from localtalker.models import Character, ChatMessage, ChatRole, Session
from localtalker.prompts import build_system_prompt, to_openai_messages


def test_system_prompt_includes_character_and_freeform_context():
    character = Character(name="Mira", personality="Warm.", instructions="Offer stew.")
    session = Session(
        character_id=character.id,
        game_context={"location": "The Hearth", "weather": "rain", "custom": {"rumor": "wolves"}},
        character_state={"mood": "busy"},
    )
    prompt = build_system_prompt(character, session)
    assert "Mira" in prompt
    assert "The Hearth" in prompt
    assert "wolves" in prompt
    assert "busy" in prompt
    assert "dialogue" in prompt


def test_openai_messages_use_structured_assistant_history():
    character = Character(name="Rook")
    session = Session(character_id=character.id)
    session.messages.append(ChatMessage(role=ChatRole.user, content="Who goes there?"))
    session.messages.append(
        ChatMessage(
            role=ChatRole.assistant,
            content="State your business.",
            reply=None,
        )
    )
    messages = to_openai_messages(character, session)
    assert messages[0]["role"] == "system"
    assert messages[1]["content"] == "Who goes there?"
    assert messages[2]["role"] == "assistant"


def test_scene_distinguishes_self_peer_speech_and_unspoken_direction():
    character = Character(name="SENTRY-9")
    session = Session(character_id=character.id, game_context={"conversation": {
        "current_speaker": "sentry", "self": {"backstory": "Held the rescue hatch."},
        "player": {"present": False, "joined": False},
        "participants": [{"key": "pip", "name": "PIP-3", "description": "Mechanic"}],
        "recent_dialogue": [
            {"speaker": "sentry", "name": "SENTRY-9", "text": "I trust your repair."},
            {"speaker": "pip", "name": "PIP-3", "text": "Then let me choose the tool."},
        ],
    }}, messages=[ChatMessage(role=ChatRole.user, content="Continue the discussion.")])
    messages = to_openai_messages(character, session)
    assert messages[1]["role"] == "assistant"
    assert "I trust your repair" in messages[1]["content"]
    assert messages[2]["role"] == "user"
    assert "[PIP-3 said]" in messages[2]["content"]
    assert "Unspoken stage direction" in messages[3]["content"]
    assert "Held the rescue hatch" in messages[0]["content"]
    assert "Then let me choose" not in messages[0]["content"]  # Transcript appears once.


def test_companion_direction_requests_actions_instead_of_speech_only():
    c = Character(name="GUIDE")
    s = Session(character_id=c.id, game_context={"interaction_mode":"companion", "conversation": {
        "current_speaker":"guide", "recent_dialogue":[{"speaker":"player","name":"Visitor","text":"Wait here."}]
    }}, messages=[ChatMessage(role=ChatRole.user,content="Respond to Visitor.")])
    messages = to_openai_messages(c,s)
    assert "actions the direction calls for" in messages[-1]["content"]
    assert "next spoken contribution" not in messages[-1]["content"]
    # The direction survives verbatim, so a companion can react to the reason it was prompted.
    assert "Respond to Visitor." in messages[-1]["content"]
    assert "Wait here." in messages[1]["content"]

def test_companion_schema_omits_unused_output_channels():
    from localtalker.prompts import reply_schema_for_messages
    c = Character(name='GUIDE')
    s = Session(character_id=c.id, game_context={'interaction_mode':'companion','conversation':{'current_speaker':'guide'}})
    schema = reply_schema_for_messages(to_openai_messages(c,s))
    assert set(schema['properties']) == {'dialogue','actions','emotion','addressee','conversation_complete'}
    assert schema['additionalProperties'] is False
