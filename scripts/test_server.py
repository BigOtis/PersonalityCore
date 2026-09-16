"""Isolated browser-test runtime. Never reads or modifies the user's data."""
import sys
import tempfile
from pathlib import Path

import uvicorn

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / "src"))
from localtalker.api import build_runtime, create_app
from localtalker.config import AppConfig
from localtalker.models import InferenceProvider, InferenceProviderKind
from tests.conftest import FakeSpeech

with tempfile.TemporaryDirectory(prefix="localtalker-browser-") as data:
    cfg = AppConfig(preferred_provider_id="mock", providers=[InferenceProvider(
        id="mock", name="Demo", kind=InferenceProviderKind.mock, model="mock-character")])
    runtime = build_runtime(cfg, Path(data) / "browser.sqlite3")
    runtime.speech = FakeSpeech()
    uvicorn.run(create_app(runtime, ROOT / "app" / "dist"), host="127.0.0.1", port=8876, log_level="warning")
