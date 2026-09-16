"""Opt-in: running service -> Kokoro input -> Whisper -> local LLM -> Kokoro reply."""
import base64
import json
import os
from pathlib import Path

import httpx
import numpy as np
import pytest
import websockets

pytestmark = [pytest.mark.smoke, pytest.mark.skipif(os.getenv("LOCALTALKER_SMOKE") != "1", reason="Set LOCALTALKER_SMOKE=1")]


async def test_real_local_voice_loop():
    base = os.getenv("LOCALTALKER_URL", "http://127.0.0.1:8765")
    async with httpx.AsyncClient(base_url=base, timeout=180) as client:
        status = (await client.get("/v1/status")).json()
        assert status["inference_ready"], "Configure a real local model before running this smoke test"
        chars = (await client.get("/v1/characters")).json()
        session = (await client.post("/v1/sessions", json={"character_id": chars[0]["id"], "title": "Speech smoke test"})).json()
        try:
            preview = await client.post("/v1/voices/preview", json={"voice_id": "af_bella", "text": "Hello Mira. Can I have a warm meal? Please keep your answer short."})
            preview.raise_for_status()
            audio = preview.json()
            assert len(base64.b64decode(audio["pcm16_b64"])) > 10000
            from localtalker.speech import wav_bytes_from_pcm16
            Path(".localtalker/validation").mkdir(parents=True, exist_ok=True)
            Path(".localtalker/validation/mic-input.wav").write_bytes(wav_bytes_from_pcm16(base64.b64decode(audio["pcm16_b64"]), audio["sample_rate"]))
            events = []
            async with websockets.connect(base.replace("http", "ws", 1) + f"/v1/sessions/{session['id']}/live", max_size=16_000_000) as ws:
                await ws.recv()
                await ws.send(json.dumps({"type": "audio", "pcm16_b64": audio["pcm16_b64"], "sample_rate": audio["sample_rate"], "channels": 1}))
                await ws.send(json.dumps({"type": "end_audio"}))
                while True:
                    import asyncio
                    event = json.loads(await asyncio.wait_for(ws.recv(), 120))
                    events.append(event)
                    assert event["type"] not in ("error", "warning"), event.get("error")
                    if event["type"] == "reply":
                        break
            transcript = next(e["transcript"] for e in events if e["type"] == "transcript")
            assert "meal" in transcript.lower(), transcript
            reply = events[-1]
            assert reply["extra"]["provider_id"] != "mock"
            assert reply["reply"]["dialogue"]
            pcm = b"".join(base64.b64decode(e["pcm16_b64"]) for e in events if e["type"] == "audio")
            assert len(pcm) > 10000
            assert np.sqrt(np.mean(np.frombuffer(pcm, dtype="<i2").astype(float) ** 2)) > 100
            stored = (await client.get(f"/v1/sessions/{session['id']}")).json()
            assert len(stored["messages"]) == 2
            artifact = Path(".localtalker/validation")
            artifact.mkdir(parents=True, exist_ok=True)
            from localtalker.speech import wav_bytes_from_pcm16
            (artifact / "real-reply.wav").write_bytes(wav_bytes_from_pcm16(pcm, 24000))
            (artifact / "real-voice-loop.json").write_text(json.dumps({"transcript": transcript, "reply": reply["reply"], "timing": reply["timing"], "provider": reply["extra"], "speech": (await client.get("/v1/status")).json()["speech"]}, indent=2), encoding="utf-8")
        finally:
            await client.delete(f"/v1/sessions/{session['id']}")
