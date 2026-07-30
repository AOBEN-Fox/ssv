from __future__ import annotations

import argparse
import hashlib
import json
import subprocess
from pathlib import Path


SOURCE_ROOT = "backend/packages/harness/deerflow"
CONFIG_FILES = [
    "__init__.py", "acp_config.py", "agents_api_config.py", "app_config.py",
    "auth_config.py", "authorization_config.py", "channel_connections_config.py",
    "checkpointer_config.py", "database_config.py", "extensions_config.py",
    "guardrails_config.py", "input_polish_config.py", "loop_detection_config.py",
    "memory_config.py", "model_config.py", "paths.py", "read_before_write_config.py",
    "reload_boundary.py", "run_events_config.py", "run_ownership_config.py",
    "runtime_paths.py", "safety_finish_reason_config.py", "sandbox_config.py",
    "scheduler_config.py", "skill_evolution_config.py", "skill_scan_config.py",
    "skills_config.py", "stream_bridge_config.py", "subagents_config.py",
    "suggestions_config.py", "summarization_config.py", "title_config.py",
    "token_budget_config.py", "token_usage_config.py", "tool_config.py",
    "tool_output_config.py", "tool_progress_config.py", "tool_search_config.py",
    "tracing_config.py",
]
FILES = [
    "__init__.py", "constants.py", "trace_context.py",
    *[f"config/{name}" for name in CONFIG_FILES],
    "models/__init__.py", "models/factory.py", "models/openai_codex_provider.py",
    "models/credential_loader.py", "reflection/__init__.py", "reflection/resolvers.py",
    "tracing/__init__.py", "tracing/factory.py", "tracing/metadata.py", "tracing/monocle.py",
]


def git_show(upstream: Path, commit: str, source_path: str) -> bytes:
    return subprocess.check_output(
        ["git", "-C", str(upstream), "show", f"{commit}:{source_path}"]
    )


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--upstream", type=Path, required=True)
    parser.add_argument("--commit", required=True)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    destination_root = root / "src/deerflow"
    manifest_files = []
    for relative_path in FILES:
        source_path = f"{SOURCE_ROOT}/{relative_path}"
        content = git_show(args.upstream, args.commit, source_path)
        destination = destination_root / relative_path
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_bytes(content)
        manifest_files.append({
            "source_path": source_path,
            "destination_path": str(destination.relative_to(root)),
            "upstream_commit": args.commit,
            "sha256": hashlib.sha256(content).hexdigest(),
            "license": "MIT",
        })
    license_bytes = git_show(args.upstream, args.commit, "LICENSE")
    (destination_root / "LICENSE").write_bytes(license_bytes)
    (destination_root / "UPSTREAM_MANIFEST.json").write_text(
        json.dumps({"upstream_commit": args.commit, "files": manifest_files}, indent=2) + "\n",
        encoding="utf-8",
    )


if __name__ == "__main__":
    main()
