import asyncio
import pytest
from localtalker.api import build_runtime
from localtalker.scenes import SceneDefinition, SceneCommand
from tests.conftest import FakeSpeech


@pytest.fixture
async def scene(tmp_path, mock_config):
    runtime = build_runtime(mock_config, tmp_path / "scenes.db")
    runtime.speech = FakeSpeech()
    await runtime.startup()
    definition = SceneDefinition(name="Test scene", members=[{"key": key,"character":{"name":key}} for key in ["a","b","c"]])
    result = await runtime.scenes.create(definition)
    try: yield result
    finally: await runtime.shutdown()


async def drain_turns(scene, count):
    q = asyncio.Queue()
    scene.subscribers.add(q)
    turns = []
    try:
        while len(turns) < count:
            event = await asyncio.wait_for(q.get(), 3)
            assert event["type"] != "error", event
            if event["type"] == "turn_end":
                turns.append(event)
                await scene.command(SceneCommand(type="playback_done", turn_id=event["turn_id"]))
        await asyncio.wait_for(scene.task, 3)
    finally: scene.subscribers.discard(q)
    return turns


async def test_droids_take_turns_with_shared_context(scene):
    await scene.command(SceneCommand(type="continue", turns=3))
    turns = await drain_turns(scene, 3)
    assert [t["speaker"] for t in turns] == ["a","b","c"]
    assert len(scene.history) == 3
    saved = await scene.runtime.store.get_session(scene.sessions["c"])
    assert [line["speaker"] for line in saved.game_context["conversation"]["recent_dialogue"]] == ["a","b"]


async def test_reset_discards_previous_run_but_preserves_cast_and_participation(scene):
    await scene.command(SceneCommand(type="join"))
    await drain_turns(scene, 1)
    await scene.command(SceneCommand(type="text", text="Remember the old run", target="a"))
    await drain_turns(scene, 1)
    sessions = dict(scene.sessions)
    assert scene.history
    await scene.command(SceneCommand(type="reset"))
    assert scene.history == [] and scene.joined and scene.sessions == sessions
    assert scene.state == "idle" and scene.task is None
    for session_id in sessions.values():
        saved = await scene.runtime.store.get_session(session_id)
        assert saved.messages == [] and saved.game_context == {} and saved.character_state == {}


async def test_join_policies_and_targeted_player_response(scene):
    await scene.command(SceneCommand(type="direct", join_policy="ignore"))
    await scene.command(SceneCommand(type="presence", player_name="Visitor"))
    assert not scene.joined
    with pytest.raises(ValueError, match="Join"):
        await scene.command(SceneCommand(type="text", text="Hello"))
    await scene.command(SceneCommand(type="direct", join_policy="invite"))
    await scene.command(SceneCommand(type="presence"))
    assert not scene.joined
    await scene.command(SceneCommand(type="join", player_name="Visitor"))
    await drain_turns(scene, 1)
    assert scene.joined
    await scene.command(SceneCommand(type="text", text="Repair the relay", target="b"))
    turns = await drain_turns(scene, 1)
    assert turns[0]["speaker"] == "b"
    assert scene.history[-2]["text"] == "Repair the relay"


async def test_direction_and_history_survive_reload(scene):
    await scene.command(SceneCommand(type="direct", goal="Protect the shuttle", context={"power":12}))
    from localtalker.scenes import SceneManager
    restored = SceneManager(scene.runtime)
    await restored.initialize()
    assert restored.scenes[scene.id].definition.goal == "Protect the shuttle"
    assert restored.scenes[scene.id].definition.context == {"power":12}


async def test_update_cast_briefs_preserves_history_and_private_backstory(scene):
    definition = scene.definition.model_copy(deep=True)
    definition.members[0].backstory = "A private fear of sealed doors."
    definition.members[0].public_description = "Rescue veteran"
    definition.members[0].character.instructions = "Listen before disagreeing."
    scene.history.append({"speaker": "a", "name": "a", "text": "Earlier agreement."})
    await scene.update_definition(definition)
    await scene.command(SceneCommand(type="continue", turns=2))
    await drain_turns(scene, 2)
    assert scene.history[0]["text"] == "Earlier agreement."
    a = await scene.runtime.store.get_session(scene.sessions["a"])
    b = await scene.runtime.store.get_session(scene.sessions["b"])
    assert a.game_context["conversation"]["self"]["backstory"] == definition.members[0].backstory
    assert "private fear" not in str(b.game_context["conversation"])
    assert (await scene.runtime.store.get_character(a.character_id)).instructions == "Listen before disagreeing."


async def test_interrupt_discards_remaining_autonomous_turns(scene):
    await scene.command(SceneCommand(type="continue", turns=30))
    await asyncio.sleep(.01)
    await scene.command(SceneCommand(type="interrupt"))
    size = len(scene.history)
    await asyncio.sleep(.1)
    assert len(scene.history) == size
    assert scene.state == "idle"


def test_scene_websocket_native_handshake_and_browser_origin(tmp_path, mock_config):
    from starlette.testclient import TestClient
    from starlette.websockets import WebSocketDisconnect
    from localtalker.api import create_app
    runtime = build_runtime(mock_config, tmp_path / "native.db")
    runtime.speech = FakeSpeech()
    with TestClient(create_app(runtime)) as client:
        response = client.post("/v1/scenes", json={"name": "Native", "members": [
            {"key": key, "character": {"name": key}} for key in ("a", "b")
        ]})
        assert response.status_code == 201
        url = f"/v1/scenes/{response.json()['id']}/live"
        with pytest.raises(WebSocketDisconnect) as rejected:
            with client.websocket_connect(url, headers={"origin": "https://unrelated.example"}):
                pass
        assert rejected.value.code == 4403
        with client.websocket_connect(url, headers={"origin": "http://127.0.0.1", "x-localtalker-client": "unreal"}) as ws:
            assert ws.receive_json()["type"] == "scene"
            ws.send_json({"type": "ping"})
            assert ws.receive_json()["type"] == "pong"


async def test_next_speaker_waits_for_matching_playback_ack(scene):
    q = asyncio.Queue()
    scene.subscribers.add(q)
    await scene.command(SceneCommand(type="continue", turns=2))
    while True:
        event = await asyncio.wait_for(q.get(), 3)
        if event["type"] == "turn_end":
            break
    first = event["turn_id"]
    await scene.command(SceneCommand(type="playback_done", turn_id="stale-turn"))
    await asyncio.sleep(.5)
    assert scene.turn_id == first
    assert len(scene.history) == 1
    await scene.command(SceneCommand(type="playback_done", turn_id=first))
    while True:
        event = await asyncio.wait_for(q.get(), 3)
        if event["type"] == "turn_start":
            assert event["speaker"] == "b"
            break
    await scene.stop()
    scene.subscribers.discard(q)


async def test_named_addressee_answers_next_without_losing_other_turns(scene):
    from localtalker.models import CharacterReply
    count = 0
    async def reply(session_id, text, emit, source):
        nonlocal count
        count += 1
        return CharacterReply(dialogue="Your thoughts?" if count == 1 else "Here is my view.", addressee="c" if count == 1 else None)
    scene.runtime.run_turn = reply
    await scene.command(SceneCommand(type="continue", turns=3))
    turns = await drain_turns(scene, 3)
    assert [t["speaker"] for t in turns] == ["a", "c", "b"]


async def test_repeated_invitations_do_not_strand_a_character_with_self_replies(scene):
    from localtalker.models import CharacterReply
    async def reply(session_id, text, emit, source):
        key = next(k for k, sid in scene.sessions.items() if sid == session_id)
        return CharacterReply(dialogue="What do you think?", addressee="b" if key == "a" else "a")
    scene.runtime.run_turn = reply
    await scene.command(SceneCommand(type="continue", turns=6))
    turns = await drain_turns(scene, 6)
    keys = [t["speaker"] for t in turns]
    assert all(left != right for left, right in zip(keys, keys[1:]))
    assert {key: keys.count(key) for key in set(keys)} == {"a": 2, "b": 2, "c": 2}


async def test_natural_pause_waits_for_every_invited_character(scene):
    from localtalker.models import CharacterReply
    async def reply(session_id, text, emit, source):
        return CharacterReply(dialogue="That settles it for me.", conversation_complete=True)
    scene.runtime.run_turn = reply
    await scene.command(SceneCommand(type="continue", turns=9))
    await asyncio.wait_for(scene.task, 4)
    assert [line["speaker"] for line in scene.history] == ["a", "b", "c"]
    assert scene.state == "idle"


@pytest.mark.parametrize("joined,expected", [(True, 1), (False, 3)])
async def test_player_invitation_yields_only_when_player_has_joined(scene, joined, expected):
    from localtalker.models import CharacterReply
    scene.present = scene.joined = joined
    async def reply(session_id, text, emit, source):
        return CharacterReply(dialogue="Would you keep it?", addressee="player")
    scene.runtime.run_turn = reply
    await scene.command(SceneCommand(type="continue", turns=3))
    await asyncio.wait_for(scene.task, 3)
    assert len(scene.history) == expected
    assert scene.state == "idle"


async def test_unstructured_planning_text_is_not_spoken_or_saved(scene, monkeypatch):
    from localtalker.inference import MockBackend, InferenceChunk
    async def stream(self, messages, cancel):
        yield InferenceChunk(delta="We need to respond as PIP. Let's plan the answer.", done=True)
    monkeypatch.setattr(MockBackend, "stream", stream)
    queue = asyncio.Queue()
    scene.subscribers.add(queue)
    await scene.command(SceneCommand(type="continue", turns=3))
    await asyncio.wait_for(scene.task, 3)
    events = []
    while not queue.empty(): events.append(queue.get_nowait())
    assert any(event["type"] == "error" for event in events)
    assert not any(event["type"] == "audio" for event in events)
    assert scene.history == []


async def test_host_can_supply_orientation_without_model_greeting(scene):
    scene.definition.greet_on_join = False
    await scene.command(SceneCommand(type="presence", player_name="Participant"))
    assert scene.joined
    assert scene.task is None
    assert not scene.history

async def test_companion_aside_cannot_execute_model_actions(scene):
    from localtalker.models import CharacterReply, LiveServerMessage
    scene.definition.context = {'interaction_mode': 'companion'}
    async def reply(session_id, text, emit, source):
        result = CharacterReply(dialogue='That door is still locked.', actions=[{'name':'open','target':'door'}])
        await emit(LiveServerMessage(type='reply', reply=result))
        return result
    scene.runtime.run_turn = reply
    queue = asyncio.Queue()
    scene.subscribers.add(queue)
    await scene.command(SceneCommand(type='continue', target='a', text='React to the locked door.'))
    await asyncio.wait_for(scene.task, 3)
    events = []
    while not queue.empty(): events.append(queue.get_nowait())
    assert next(e for e in events if e['type']=='reply')['reply']['actions'] == []
    assert scene.history[-1]['reply']['actions'] == []
    saved = await scene.runtime.store.get_session(scene.sessions['a'])
    assert saved.game_context['speech_only'] is True


async def test_companion_current_instruction_is_explicit_and_history_bounded(scene):
    from localtalker.models import CharacterReply
    scene.definition.context = {'interaction_mode':'companion'}
    scene.history = [{'speaker':'player','text':'Old command '+str(i)+'x'*400} for i in range(12)]
    captured=[]
    async def reply(session_id,text,emit,source):
        captured.append(text)
        return CharacterReply(dialogue='Printing it.')
    scene.runtime.run_turn=reply
    await scene.player_turn('Print my certificate.','a')
    await asyncio.wait_for(scene.task,3)
    assert "Current player instruction: 'Print my certificate.'" in captured[0]
    assert 'never replay' in captured[0]
    saved=await scene.runtime.store.get_session(scene.sessions['a'])
    recent=saved.game_context['conversation']['recent_dialogue']
    assert len(recent)<6 and recent[-1]['text']=='Print my certificate.'


async def test_direct_can_patch_context_without_stopping(scene):
    await scene.command(SceneCommand(type="continue", turns=3))
    await asyncio.sleep(0.05)
    assert scene.task is not None and not scene.task.done()
    await scene.command(SceneCommand(type="direct", context={"power": 1}, stop=False))
    assert scene.definition.context["power"] == 1
    assert scene.task is not None and not scene.task.done()
    await scene.command(SceneCommand(type="interrupt"))


async def test_scripted_speak_skips_the_model_and_still_voices(scene):
    from localtalker.models import CharacterReply
    called = []
    async def reply(session_id, text, emit, source):
        called.append(text)
        return CharacterReply(dialogue='I should not have been asked.')
    scene.runtime.run_turn = reply
    queue = asyncio.Queue()
    scene.subscribers.add(queue)
    await scene.command(SceneCommand(type='speak', target='a', text='The door closed. I am on this side.'))
    events = []
    while True:
        event = await asyncio.wait_for(queue.get(), 3)
        events.append(event)
        if event['type'] == 'turn_end':
            await scene.command(SceneCommand(type='playback_done', turn_id=event['turn_id']))
            break
    await asyncio.wait_for(scene.task, 3)
    while not queue.empty():
        events.append(queue.get_nowait())
    assert called == []
    assert scene.history[-1]['text'] == 'The door closed. I am on this side.'
    assert scene.history[-1]['scripted'] is True
    assert scene.history[-1]['reply']['actions'] == []
    assert any(event['type'] == 'audio' for event in events)
    assert next(e for e in events if e['type'] == 'reply')['reply']['dialogue'] == 'The door closed. I am on this side.'
    assert scene.runtime.speech.received_text == ['The door closed. I am on this side.']


async def test_scripted_and_announcer_lines_stay_out_of_model_history(scene):
    from localtalker.models import CharacterReply
    scene.definition.context = {'interaction_mode': 'companion'}
    scene.history = [
        {'speaker': 'announcer', 'name': 'Institute PA', 'text': 'Welcome, volunteer.', 'scripted': True},
        {'speaker': 'guide', 'name': 'GUIDE', 'text': 'Hello. I am GUIDE.', 'scripted': True},
        {'speaker': 'player', 'name': 'Participant', 'text': 'Pick up the can.'},
        {'speaker': 'guide', 'name': 'GUIDE', 'text': 'Picking it up.'},
    ]
    captured = []
    async def reply(session_id, text, emit, source):
        captured.append(await scene.runtime.store.get_session(session_id))
        return CharacterReply(dialogue='Done.')
    scene.runtime.run_turn = reply
    await scene.player_turn('Put it on the table.', 'a')
    await asyncio.wait_for(scene.task, 3)
    recent = captured[0].game_context['conversation']['recent_dialogue']
    speakers = [line['speaker'] for line in recent]
    assert 'announcer' not in speakers
    assert all(not line.get('scripted') for line in recent)
    assert speakers[-1] == 'player'
    assert recent[-1]['text'] == 'Put it on the table.'
