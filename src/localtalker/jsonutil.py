from __future__ import annotations

import json
import re
from typing import Any

from localtalker.models import ActionCue, CharacterReply

_DIALOGUE_KEY = re.compile(r'"dialogue"\s*:\s*"')


def extract_json_object(text: str) -> dict[str, Any] | None:
    """Best-effort object extraction from a model response."""
    text = text.strip()
    if not text:
        return None
    candidates = [text]
    fenced = re.search(r"```(?:json)?\s*(\{.*\})\s*```", text, re.DOTALL)
    if fenced:
        candidates.insert(0, fenced.group(1))
    start = text.find("{")
    end = text.rfind("}")
    if start >= 0 and end > start:
        candidates.append(text[start : end + 1])
    for candidate in candidates:
        try:
            value = json.loads(candidate)
        except json.JSONDecodeError:
            continue
        if isinstance(value, dict):
            return value
    return None


def incremental_dialogue(partial: str) -> str | None:
    """Use Pydantic's streaming JSON parser, including Unicode escapes."""
    from pydantic_core import from_json

    start = partial.find("{")
    if start < 0:
        return None
    try:
        obj = from_json(partial[start:], allow_partial="trailing-strings")
    except ValueError:
        return None
    value = obj.get("dialogue") if isinstance(obj, dict) else None
    return value if isinstance(value, str) else None


def parse_character_reply(text: str) -> CharacterReply:
    obj = extract_json_object(text)
    if not obj:
        if text.lstrip().startswith(("{", "```")):
            raise ValueError("The model returned incomplete or invalid JSON. Increase max tokens or try a different model.")
        return CharacterReply.from_dialogue(text)
    return CharacterReply.model_validate(obj)
