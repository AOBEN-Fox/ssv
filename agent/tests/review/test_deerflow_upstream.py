from __future__ import annotations

import hashlib
import json
from pathlib import Path


AGENT_ROOT = Path(__file__).resolve().parents[2]
MANIFEST = AGENT_ROOT / "src/deerflow/UPSTREAM_MANIFEST.json"
UPSTREAM_COMMIT = "b68e1c686a0cb5a3780089d27354354533451d8e"


def test_manifest_hashes_and_license() -> None:
    manifest = json.loads(MANIFEST.read_text(encoding="utf-8"))
    assert manifest["upstream_commit"] == UPSTREAM_COMMIT
    assert (AGENT_ROOT / "src/deerflow/LICENSE").is_file()
    for item in manifest["files"]:
        destination = AGENT_ROOT / item["destination_path"]
        assert hashlib.sha256(destination.read_bytes()).hexdigest() == item["sha256"]
        assert item["license"] == "MIT"


def test_factory_entrypoints_import() -> None:
    from deerflow.config.app_config import AppConfig
    from deerflow.config.model_config import ModelConfig
    from deerflow.models.factory import create_chat_model
    from deerflow.reflection import resolve_class

    assert AppConfig and ModelConfig and create_chat_model and resolve_class


def test_upstream_subtree_has_no_ssv_business_import() -> None:
    for path in (AGENT_ROOT / "src/deerflow").rglob("*.py"):
        assert "ssv_agent" not in path.read_text(encoding="utf-8")
