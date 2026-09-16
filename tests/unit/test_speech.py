import numpy as np
import pytest
from localtalker.speech import pcm16_to_float32_16k, decode_pcm16
from localtalker.jsonutil import incremental_dialogue, parse_character_reply


def test_stereo_resampling_preserves_signal_and_duration():
    samples = (np.sin(np.arange(48000) * 2 * np.pi * 440 / 48000) * 12000).astype("<i2")
    result = pcm16_to_float32_16k(np.column_stack([samples, samples]).tobytes(), 48000, 2)
    assert len(result) == 16000
    assert .2 < np.sqrt(np.mean(result ** 2)) < .3
    assert np.argmax(abs(np.fft.rfft(result))) == 440


def test_invalid_audio_is_rejected():
    with pytest.raises(ValueError):
        pcm16_to_float32_16k(b"a", 16000, 1)
    with pytest.raises(ValueError):
        pcm16_to_float32_16k(b"", 0, 1)
    with pytest.raises(ValueError):
        decode_pcm16("not valid base64!!!")


def test_explicit_voice_assets_never_fall_back_to_a_download(tmp_path, monkeypatch):
    from localtalker.config import SpeechConfig
    from localtalker.speech import SpeechService
    import localtalker.speech as speech
    monkeypatch.setattr(speech, "ensure_kokoro_files", lambda: pytest.fail("Attempted a download"))
    service = SpeechService(SpeechConfig(kokoro_model_path=str(tmp_path / "missing.onnx"),
                                        kokoro_voices_path=str(tmp_path / "voices.bin")))
    with pytest.raises(FileNotFoundError, match="Bundled Kokoro"):
        service._load_kokoro()


def test_partial_unicode_is_never_spoken_as_escape_digits():
    assert incremental_dialogue(r'{"dialogue":"Caf\u00e9"}') == "Café"
    partial = incremental_dialogue(r'{"dialogue":"Caf\u00')
    assert partial is None or partial == "Caf"
    with pytest.raises(ValueError):
        parse_character_reply('{"dialogue":{},"actions":"oops"}')
