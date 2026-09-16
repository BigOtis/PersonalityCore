from __future__ import annotations

import argparse
import logging
import os
import sys
from pathlib import Path

import uvicorn

from localtalker.api import create_app, build_runtime
from localtalker.config import load_config
from localtalker.paths import db_path, user_data_dir


def main(argv: list[str] | None = None) -> None:
    parser = argparse.ArgumentParser(prog="localtalker", description="LocalTalker runtime and developer console.")
    sub = parser.add_subparsers(dest="command")

    serve = sub.add_parser("serve", help="Start the LocalTalker HTTP/WebSocket runtime")
    serve.add_argument("--host", default=None)
    serve.add_argument("--port", type=int, default=None)
    serve.add_argument("--reload", action="store_true")
    serve.add_argument("--data-dir", default=None)
    serve.add_argument("--mock-only", action="store_true", help="Force the mock inference provider")

    sub.add_parser("info", help="Print data paths and config location")

    models = sub.add_parser("models", help="List or install recommended local models")
    models.add_argument("action", choices=["list", "install"], nargs="?", default="list")
    models.add_argument("model_id", nargs="?", default="muse-glimmer")

    args = parser.parse_args(argv)
    if args.command in (None, "serve"):
        _serve(args)
        return
    if args.command == "info":
        _info()
        return
    if args.command == "models":
        _models(args)
        return
    parser.print_help()


def _configure_logging() -> None:
    logging.basicConfig(
        level=logging.INFO,
        format="%(asctime)s %(levelname)s %(name)s: %(message)s",
    )


def _serve(args: argparse.Namespace) -> None:
    _configure_logging()
    if getattr(args, "data_dir", None):
        os.environ["LOCALTALKER_HOME"] = str(Path(args.data_dir).resolve())
    cfg = load_config()
    if getattr(args, "mock_only", False):
        cfg.preferred_provider_id = "mock"
        for provider in cfg.providers:
            provider.enabled = provider.id == "mock"
    host = getattr(args, "host", None) or cfg.host
    port = getattr(args, "port", None) or cfg.port
    static_dir = _frontend_dir()
    runtime = build_runtime(cfg, db_path())
    app = create_app(runtime, static_dir=static_dir)

    print(f"LocalTalker runtime on http://{host}:{port}")
    print(f"Data directory: {user_data_dir()}")
    uvicorn.run(app, host=host, port=port, reload=getattr(args, "reload", False), log_level="info")


def _frontend_dir() -> Path | None:
    if getattr(sys, "frozen", False):
        return Path(sys._MEIPASS) / "ui"
    here = Path(__file__).resolve()
    candidates = [
        here.parents[2] / "app" / "dist",
        Path.cwd() / "app" / "dist",
    ]
    for path in candidates:
        if (path / "index.html").exists():
            return path
    return None


def _models(args: argparse.Namespace) -> None:
    from localtalker.catalog import recommended_status, get_recommended
    from localtalker.config import load_config, save_config
    from localtalker.install import install_sync

    if args.action == "list":
        for item in recommended_status():
            mark = "installed" if item["installed"] else "not downloaded"
            print(f"{item['id']}: {item['name']} ({mark})")
            print(f"  {item['description']}")
            if item["path"]:
                print(f"  {item['path']}")
        return
    model = get_recommended(args.model_id)
    dest = install_sync(model) if not model.installed() else model.dest()
    cfg = load_config()
    cfg.llama_model_path = str(dest)
    cfg.llama_ctx = min(cfg.llama_ctx, model.context)
    cfg.preferred_provider_id = "llamacpp"
    from localtalker.inference import _resolve_llama_server

    binary = _resolve_llama_server(cfg.llama_server_path, dest)
    if binary:
        cfg.llama_server_path = str(binary)
    for provider in cfg.providers:
        if provider.id == "llamacpp":
            provider.model = "muse-glimmer"
            provider.temperature = model.temperature
            provider.max_tokens = max(provider.max_tokens, 800)
            provider.enabled = True
    save_config(cfg)
    print(f"Ready: {dest}")
    if cfg.llama_server_path:
        print(f"llama-server: {cfg.llama_server_path}")
    print("Restart LocalTalker or send a message to load Muse Glimmer on the GPU.")


def _info() -> None:
    cfg = load_config()
    print(f"data: {user_data_dir()}")
    print(f"db: {db_path()}")
    print(f"host: {cfg.host}:{cfg.port}")
    for provider in cfg.providers:
        print(f"provider {provider.id}: {provider.kind.value} {provider.base_url} model={provider.model or '-'}")


if __name__ == "__main__":
    main(sys.argv[1:])
