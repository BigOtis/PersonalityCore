from localtalker.jsonutil import extract_json_object, incremental_dialogue, parse_character_reply


def test_incremental_dialogue_grows_with_partial_json():
    partial = '{"dialogue":"Hello there, traveler'
    assert incremental_dialogue(partial) == "Hello there, traveler"
    complete = partial + '","emotion":"warm"}'
    assert incremental_dialogue(complete) == "Hello there, traveler"


def test_incremental_dialogue_unescapes():
    assert incremental_dialogue(r'{"dialogue":"line\nbreak"}') == "line\nbreak"


def test_parse_character_reply_from_fenced_json():
    raw = """```json
    {"dialogue":"Stand aside.","emotion":"stern","intent":"warn","actions":[{"name":"point","target":"gate"}]}
    ```"""
    reply = parse_character_reply(raw)
    assert reply.dialogue == "Stand aside."
    assert reply.emotion == "stern"
    assert reply.intent == "warn"
    assert reply.actions[0].name == "point"
    assert reply.actions[0].target == "gate"


def test_parse_character_reply_falls_back_to_plain_text():
    reply = parse_character_reply("Just a spoken line.")
    assert reply.dialogue == "Just a spoken line."
    assert reply.actions == []


def test_extract_json_object_ignores_leading_prose():
    obj = extract_json_object('Sure. {"dialogue":"Yes.","emotion":"calm"}')
    assert obj is not None
    assert obj["dialogue"] == "Yes."
