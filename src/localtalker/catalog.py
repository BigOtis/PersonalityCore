from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path

from localtalker.paths import extra_data_dirs, models_dir


@dataclass(frozen=True)
class RecommendedModel:
    id: str
    name: str
    repo_id: str
    filename: str
    size_bytes: int
    min_vram_gb: int
    context: int
    temperature: float
    description: str

    def dest(self) -> Path:
        for path in self.candidate_paths():
            if path.is_file() and path.stat().st_size > self.size_bytes * 0.98:
                return path
        return models_dir() / self.filename

    def candidate_paths(self) -> list[Path]:
        paths = [models_dir() / self.filename]
        for folder in extra_data_dirs():
            extra = folder / "models" / self.filename
            if extra not in paths:
                paths.append(extra)
        return paths

    def installed(self) -> bool:
        path = self.dest()
        return path.is_file() and path.stat().st_size > self.size_bytes * 0.98


MUSE_GLIMMER_Q3 = RecommendedModel(
    id="muse-glimmer",
    name="Muse Glimmer 30B",
    repo_id="unsloth/Muse-Glimmer-30B-GGUF",
    filename="Muse-Glimmer-30B-UD-Q3_K_XL.gguf",
    size_bytes=13_360_983_072,
    min_vram_gb=16,
    context=4096,
    temperature=1.0,
    description=(
        "Meta's local agent model. This Unsloth Q3 quant is the strongest Glimmer "
        "build that fits a 16 GB NVIDIA card for spoken character work. Vision and "
        "DFlash are omitted so the GPU has room for context."
    ),
)

RECOMMENDED = (MUSE_GLIMMER_Q3,)


def recommended_status() -> list[dict]:
    items = []
    for model in RECOMMENDED:
        dest = model.dest()
        have = dest.stat().st_size if dest.is_file() else 0
        items.append(
            {
                "id": model.id,
                "name": model.name,
                "filename": model.filename,
                "repo_id": model.repo_id,
                "description": model.description,
                "size_bytes": model.size_bytes,
                "min_vram_gb": model.min_vram_gb,
                "installed": model.installed(),
                "path": str(dest) if dest.exists() else "",
                "bytes_on_disk": have,
            }
        )
    return items


def get_recommended(model_id: str) -> RecommendedModel:
    for model in RECOMMENDED:
        if model.id == model_id:
            return model
    raise KeyError(f"Unknown recommended model: {model_id}")
