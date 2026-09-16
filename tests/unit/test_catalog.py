from pathlib import Path

from localtalker.catalog import MUSE_GLIMMER_Q3, get_recommended, recommended_status
from localtalker.config import AppConfig
from localtalker.inference import MIN_GLIMMER_LLAMA_BUILD, _llama_build_number, _resolve_llama_server, llama_server_args


def test_muse_glimmer_is_the_16gb_recommendation():
    model = get_recommended("muse-glimmer")
    assert model is MUSE_GLIMMER_Q3
    assert model.min_vram_gb == 16
    assert model.filename.endswith("UD-Q3_K_XL.gguf")
    assert any(item["id"] == "muse-glimmer" for item in recommended_status())


def test_llama_server_args_enable_glimmer_chat_template():
    cfg = AppConfig(llama_port=8090, llama_ctx=4096, llama_n_gpu_layers=99)
    args = llama_server_args(cfg, Path("llama-server.exe"), Path("Muse-Glimmer-30B-UD-Q3_K_XL.gguf"))
    assert "--jinja" in args
    assert args[args.index("-fa") + 1] == "on"
    assert args[args.index("--alias") + 1] == "muse-glimmer"
    assert args[args.index("--chat-template-kwargs") + 1] == '{"reasoning_strength":"low"}'
    assert args[args.index("--reasoning-budget") + 1] == "128"


def test_resolve_llama_server_prefers_glimmer_capable_build(tmp_path, monkeypatch):
    old = tmp_path / "legacy" / "llama-server.exe"
    new = tmp_path / "llama-b10610" / "llama-server.exe"
    old.parent.mkdir()
    new.parent.mkdir()
    old.write_bytes(b"old")
    new.write_bytes(b"new")
    monkeypatch.setattr("localtalker.inference.runtimes_dir", lambda: tmp_path)
    monkeypatch.setattr("localtalker.inference.extra_data_dirs", lambda: [])
    monkeypatch.setattr("localtalker.inference.shutil.which", lambda _name: None)
    chosen = _resolve_llama_server(str(old), Path("Muse-Glimmer-30B-UD-Q3_K_XL.gguf"))
    assert chosen == new.resolve()
    assert _llama_build_number(new) >= MIN_GLIMMER_LLAMA_BUILD
