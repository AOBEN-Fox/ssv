from __future__ import annotations

from pathlib import Path

import pytest

from ssv_agent.config import load_config


def test_load_config_uses_yaml_values(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.delenv("SSV_CONFIG_PATH", raising=False)
    monkeypatch.delenv("REDIS_HOST", raising=False)
    monkeypatch.delenv("REDIS_PORT", raising=False)
    monkeypatch.delenv("SSV_LOG_LEVEL", raising=False)
    monkeypatch.delenv("SSV_DISPLAY_SINK", raising=False)
    path = tmp_path / "ssv.yaml"
    path.write_text(
        """
version: "9.9"
redis:
  host: "redis.local"
  port: 6380
  stream_key: "custom:events"
pipeline:
  analysis_fps: 7
inference:
  target_class: "helmet"
""".strip(),
        encoding="utf-8",
    )

    cfg = load_config(path)

    assert cfg.version == "9.9"
    assert cfg.redis.host == "redis.local"
    assert cfg.redis.port == 6380
    assert cfg.redis.stream_key == "custom:events"
    assert cfg.pipeline.analysis_fps == 7
    assert cfg.inference.target_class == "helmet"


def test_load_config_reads_event_evidence_review_and_model_config(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    monkeypatch.delenv("SSV_EVENT_HELMET_VIOLATION_ENABLED", raising=False)
    monkeypatch.delenv("SSV_EVIDENCE_OUTPUT_DIR", raising=False)
    monkeypatch.delenv("SSV_REVIEW_PROVIDER", raising=False)
    path = tmp_path / "ssv.yaml"
    path.write_text(
        """
events:
  helmet_violation:
    enabled: true
    trigger_class: "head"
    publish_detection_events: false
evidence:
  output_dir: "tmp/evidence"
reviews:
  output_dir: "tmp/reviews"
agent:
  model:
    provider: "right_codes"
    base_url: "https://right.codes/codex/v1"
    model: "gpt-5.5"
    api_key_env: "RIGHT_CODES_API_KEY"
    timeout_seconds: 30
""".strip(),
        encoding="utf-8",
    )

    cfg = load_config(path)

    assert cfg.events.helmet_violation.enabled is True
    assert cfg.events.helmet_violation.trigger_class == "head"
    assert cfg.events.helmet_violation.publish_detection_events is False
    assert cfg.evidence.output_dir == "tmp/evidence"
    assert cfg.reviews.output_dir == "tmp/reviews"
    assert cfg.agent.model.provider == "right_codes"
    assert cfg.agent.model.base_url == "https://right.codes/codex/v1"
    assert cfg.agent.model.model == "gpt-5.5"
    assert cfg.agent.model.api_key_env == "RIGHT_CODES_API_KEY"
    assert cfg.agent.model.timeout_seconds == 30


def test_load_config_applies_environment_overrides(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    path = tmp_path / "ssv.yaml"
    path.write_text("""
redis:
  host: yaml-host
  port: 1111
""".lstrip(), encoding="utf-8")
    monkeypatch.setenv("REDIS_HOST", "env-host")
    monkeypatch.setenv("REDIS_PORT", "2222")
    monkeypatch.setenv("SSV_LOG_LEVEL", "DEBUG")
    monkeypatch.setenv("SSV_DISPLAY_SINK", "fakesink")

    cfg = load_config(path)

    assert cfg.redis.host == "env-host"
    assert cfg.redis.port == 2222
    assert cfg.logging.python_log_level == "DEBUG"
    assert cfg.display.sink == "fakesink"


def test_load_config_missing_explicit_path_raises(tmp_path: Path) -> None:
    with pytest.raises(FileNotFoundError):
        load_config(tmp_path / "missing.yaml")
