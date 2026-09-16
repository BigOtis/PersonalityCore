import pytest
from httpx import ASGITransport, AsyncClient

from localtalker.api import build_runtime, create_app
from localtalker.scenes import SceneCommand
from tests.conftest import FakeSpeech
from tests.integration.test_scenes import drain_turns


@pytest.fixture
async def client(tmp_path, mock_config):
    runtime = build_runtime(mock_config, tmp_path / "groups.db")
    runtime.speech = FakeSpeech()
    await runtime.startup()
    app = create_app(runtime, start=False)
    transport = ASGITransport(app=app)
    try:
        async with AsyncClient(transport=transport, base_url="http://test") as http:
            yield http, runtime
    finally:
        await runtime.shutdown()


async def test_group_chat_reuses_characters_and_takes_turns(client):
    http, runtime = client
    characters = (await http.get("/v1/characters")).json()
    mira, rook = characters[0], characters[1]
    before = len(characters)
    created = await http.post(
        "/v1/groups",
        json={"character_ids": [mira["id"], rook["id"]], "player_name": "You"},
    )
    assert created.status_code == 201
    group = created.json()
    assert group["kind"] == "group"
    assert len(group["definition"]["members"]) == 2
    assert {member["character_id"] for member in group["definition"]["members"]} == {mira["id"], rook["id"]}
    assert len((await http.get("/v1/characters")).json()) == before

    listed = (await http.get("/v1/groups")).json()
    assert listed[0]["id"] == group["id"]

    scene = runtime.scenes.scenes[group["id"]]
    await scene.command(SceneCommand(type="join", player_name="You"))
    assert scene.joined
    assert scene.task is None
    await scene.command(SceneCommand(type="text", text="What should we do tonight?", target="all"))
    turns = await drain_turns(scene, 2)
    assert {turn["speaker"] for turn in turns} == {mira["id"], rook["id"]}
    assert scene.history[0]["speaker"] == "player"
    assert scene.history[0]["text"] == "What should we do tonight?"
    names = {line["name"] for line in scene.history[1:]}
    assert mira["name"] in names and rook["name"] in names


async def test_group_chat_can_target_one_character(client):
    http, runtime = client
    characters = (await http.get("/v1/characters")).json()
    mira, rook = characters[0], characters[1]
    group = (await http.post("/v1/groups", json={"character_ids": [mira["id"], rook["id"]]})).json()
    scene = runtime.scenes.scenes[group["id"]]
    await scene.command(SceneCommand(type="join", player_name="You"))
    await scene.command(SceneCommand(type="text", text="Rook, is the gate open?", target=rook["id"]))
    turns = await drain_turns(scene, 1)
    assert turns[0]["speaker"] == rook["id"]
    assert turns[0]["name"] == rook["name"]


async def test_delete_group_keeps_characters(client):
    http, _runtime = client
    characters = (await http.get("/v1/characters")).json()
    group = (
        await http.post("/v1/groups", json={"character_ids": [characters[0]["id"], characters[1]["id"]]})
    ).json()
    deleted = await http.delete(f"/v1/groups/{group['id']}")
    assert deleted.status_code == 200
    assert (await http.get("/v1/groups")).json() == []
    remaining = (await http.get("/v1/characters")).json()
    assert {item["id"] for item in remaining} >= {characters[0]["id"], characters[1]["id"]}
