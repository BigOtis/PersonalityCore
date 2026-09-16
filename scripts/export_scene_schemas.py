"""Export the source-of-truth scene types for engine tooling and coding agents."""
import json
from pathlib import Path
from localtalker.scenes import SceneDefinition, SceneCommand

destination = Path(__file__).resolve().parents[1] / "protocol"
for name, model in [("scene-definition.v1", SceneDefinition), ("scene-command.v1", SceneCommand)]:
    (destination / f"{name}.schema.json").write_text(
        json.dumps(model.model_json_schema(), indent=2) + "\n", encoding="utf-8"
    )
