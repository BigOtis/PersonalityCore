from __future__ import annotations

import logging
import threading
from pathlib import Path
from typing import Any

import httpx

from localtalker.catalog import RecommendedModel, get_recommended

log = logging.getLogger("localtalker.install")

_lock = threading.Lock()
_job: dict[str, Any] = {
    "id": None,
    "status": "idle",
    "error": None,
    "path": "",
    "bytes_on_disk": 0,
    "size_bytes": 0,
}


def job_status() -> dict[str, Any]:
    with _lock:
        current = dict(_job)
    if current.get("id"):
        try:
            model = get_recommended(current["id"])
            dest = model.dest()
            current["bytes_on_disk"] = dest.stat().st_size if dest.is_file() else 0
            current["size_bytes"] = model.size_bytes
            current["path"] = str(dest)
            if model.installed() and current["status"] == "downloading":
                current["status"] = "ready"
        except Exception:
            pass
    return current


def start_install(model_id: str) -> dict[str, Any]:
    model = get_recommended(model_id)
    with _lock:
        if _job["status"] == "downloading" and _job["id"] == model_id:
            return job_status()
        _job.update({"id": model_id, "status": "downloading", "error": None, "path": str(model.dest())})
    thread = threading.Thread(target=_download, args=(model,), daemon=True, name=f"install-{model_id}")
    thread.start()
    return job_status()


def install_sync(model: RecommendedModel) -> Path:
    dest = model.dest()
    dest.parent.mkdir(parents=True, exist_ok=True)
    if model.installed():
        return dest
    log.info("Downloading %s from %s", model.filename, model.repo_id)
    downloaded = _http_download(model, dest)
    if not dest.is_file() or dest.stat().st_size < model.size_bytes * 0.98:
        raise RuntimeError(f"Download incomplete: {dest}")
    log.info("Installed %s (%s bytes)", dest, dest.stat().st_size)
    return dest


def _http_download(model: RecommendedModel, dest: Path) -> Path:
    url = f"https://huggingface.co/{model.repo_id}/resolve/main/{model.filename}"
    tmp = dest.with_suffix(dest.suffix + ".partial")
    last_error: Exception | None = None
    for attempt in range(1, 8):
        already = tmp.stat().st_size if tmp.exists() else 0
        if already >= model.size_bytes * 0.98:
            break
        headers = {"User-Agent": "LocalTalker/0.1"}
        if already:
            headers["Range"] = f"bytes={already}-"
        try:
            with httpx.stream("GET", url, headers=headers, follow_redirects=True, timeout=None) as response:
                response.raise_for_status()
                mode = "ab" if already and response.status_code == 206 else "wb"
                written = already if mode == "ab" else 0
                with tmp.open(mode) as handle:
                    for chunk in response.iter_bytes(1024 * 1024):
                        handle.write(chunk)
                        written += len(chunk)
                        if written and written % (256 * 1024 * 1024) < 1024 * 1024:
                            log.info("Downloaded %.1f / %.1f GB", written / 1e9, model.size_bytes / 1e9)
            last_error = None
            break
        except (httpx.HTTPError, OSError) as exc:
            last_error = exc
            log.warning("Download attempt %s failed: %s", attempt, exc)
    if last_error and (not tmp.exists() or tmp.stat().st_size < model.size_bytes * 0.98):
        raise last_error
    tmp.replace(dest)
    return dest


def _download(model: RecommendedModel) -> None:
    try:
        dest = install_sync(model)
        with _lock:
            _job.update({"status": "ready", "path": str(dest), "error": None})
    except Exception as exc:
        log.exception("Model install failed")
        with _lock:
            _job.update({"status": "error", "error": str(exc)})
