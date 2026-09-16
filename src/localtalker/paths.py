from __future__ import annotations

import os
from pathlib import Path


APP_NAME = "LocalTalker"


def user_data_dir() -> Path:
    if override := os.environ.get("LOCALTALKER_HOME"):
        path = Path(override)
    elif os.name == "nt":
        root = os.environ.get("LOCALAPPDATA") or str(Path.home())
        path = Path(root) / APP_NAME
    else:
        path = Path.home() / ".local" / "share" / "localtalker"
    path.mkdir(parents=True, exist_ok=True)
    return path


def models_dir() -> Path:
    path = user_data_dir() / "models"
    path.mkdir(parents=True, exist_ok=True)
    return path


def speech_dir() -> Path:
    path = user_data_dir() / "speech"
    path.mkdir(parents=True, exist_ok=True)
    return path


def kokoro_dir() -> Path:
    path = speech_dir() / "kokoro"
    path.mkdir(parents=True, exist_ok=True)
    return path


def whisper_dir() -> Path:
    path = speech_dir() / "whisper"
    path.mkdir(parents=True, exist_ok=True)
    return path


def runtimes_dir() -> Path:
    path = user_data_dir() / "runtimes"
    path.mkdir(parents=True, exist_ok=True)
    return path


def repo_data_dir() -> Path | None:
    candidate = Path(__file__).resolve().parents[2] / ".localtalker"
    if candidate.is_dir():
        return candidate
    return None


def extra_data_dirs() -> list[Path]:
    extras: list[Path] = []
    home = user_data_dir().resolve()
    repo = repo_data_dir()
    if repo and repo.resolve() != home:
        extras.append(repo)
    return extras


def db_path() -> Path:
    return user_data_dir() / "localtalker.sqlite3"


def config_path() -> Path:
    return user_data_dir() / "config.json"
