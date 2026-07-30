from __future__ import annotations

import os
from pathlib import Path

import yaml
from pydantic import BaseModel, ConfigDict, Field, model_validator


class LoggingConfig(BaseModel):
    cpp_debug_level: str = "ssv*:4"
    python_log_level: str = "INFO"


class RedisConfig(BaseModel):
    host: str = "localhost"
    port: int = 6379
    db: int = 0
    stream_key: str = "ssv:events"
    review_candidate_stream: str = "ssv:review-candidates"
    review_result_stream: str = "ssv:review-results"
    consumer_group: str = "ssv-agent"


class DisplayConfig(BaseModel):
    enabled: bool = False
    overlay: bool = False
    fps: int = 30
    sink: str = "autovideosink"


class PipelineConfig(BaseModel):
    check_timeout: str = "30s"
    analysis_fps: int = 5
    frame_width: int = 640
    frame_height: int = 480


class InferenceConfig(BaseModel):
    runtime: str = "auto"
    model_path: str = ""
    confidence_threshold: float = 0.5
    device: str = "auto"
    device_id: int = 0
    precision: str = "auto"
    model_family: str = "yolo"
    output_format: str = "auto"
    target_class: str = "person"
    label_map: str = "config/model-labels/coco80.txt"


class TrackingConfig(BaseModel):
    enabled: bool = True
    frame_rate: int = 30
    track_threshold: float = 0.5
    track_buffer: int = 30
    match_threshold: float = 0.3
    mock_track: bool = False


class AgentConfig(BaseModel):
    state_machine_timeout: int = 300
    max_retries: int = 3
    review_model: str = ""
    models: list["ReviewModelConfig"] = Field(default_factory=list)


class ReviewModelConfig(BaseModel):
    model_config = ConfigDict(extra="allow")

    name: str
    use: str
    model: str
    supports_vision: bool = False


class ArtifactsConfig(BaseModel):
    events_root: Path = Path("artifacts/events")


class ReviewConfig(BaseModel):
    enabled: bool = False
    automatic_decision_min_confidence: float = Field(default=0.80, ge=0.0, le=1.0)


class SsvConfig(BaseModel):
    version: str = "1.0"
    logging: LoggingConfig = Field(default_factory=LoggingConfig)
    redis: RedisConfig = Field(default_factory=RedisConfig)
    display: DisplayConfig = Field(default_factory=DisplayConfig)
    pipeline: PipelineConfig = Field(default_factory=PipelineConfig)
    inference: InferenceConfig = Field(default_factory=InferenceConfig)
    tracking: TrackingConfig = Field(default_factory=TrackingConfig)
    artifacts: ArtifactsConfig = Field(default_factory=ArtifactsConfig)
    review: ReviewConfig = Field(default_factory=ReviewConfig)
    agent: AgentConfig = Field(default_factory=AgentConfig)
    sources: list[dict] = Field(default_factory=list)

    @model_validator(mode="after")
    def validate_review_model(self) -> "SsvConfig":
        if not self.review.enabled:
            return self

        selected = [model for model in self.agent.models if model.name == self.agent.review_model]
        if len(selected) != 1:
            raise ValueError(
                "review.enabled=true 时 agent.review_model 必须唯一匹配 agent.models[].name"
            )
        if not selected[0].supports_vision:
            raise ValueError("agent.review_model 必须配置 supports_vision=true")
        return self


def _apply_env_overrides(cfg: SsvConfig) -> None:
    """Override deployment-sensitive config fields from environment variables."""
    if v := os.environ.get("REDIS_HOST"):
        cfg.redis.host = v
    if v := os.environ.get("REDIS_PORT"):
        cfg.redis.port = int(v)


def load_config(path: str | Path | None = None) -> SsvConfig:
    """Load configuration from YAML file.

    Search order: explicit path -> SSV_CONFIG_PATH env -> ssv.yaml -> config/ssv.yaml -> defaults.
    Environment variables REDIS_HOST and REDIS_PORT override corresponding YAML values.
    """
    cfg: SsvConfig | None = None

    if path is not None:
        p = Path(path)
        if p.exists():
            with open(p) as f:
                data = yaml.safe_load(f) or {}
            cfg = SsvConfig.model_validate(data)
        else:
            raise FileNotFoundError(f"Config file not found: {p}")

    if cfg is None:
        env_path = os.environ.get("SSV_CONFIG_PATH")
        if env_path and Path(env_path).exists():
            with open(env_path) as f:
                data = yaml.safe_load(f) or {}
            cfg = SsvConfig.model_validate(data)

    if cfg is None:
        local = Path("ssv.yaml")
        if local.exists():
            with open(local) as f:
                data = yaml.safe_load(f) or {}
            cfg = SsvConfig.model_validate(data)

    if cfg is None:
        config_local = Path("config/ssv.yaml")
        if config_local.exists():
            with open(config_local) as f:
                data = yaml.safe_load(f) or {}
            cfg = SsvConfig.model_validate(data)

    if cfg is None:
        cfg = SsvConfig()

    _apply_env_overrides(cfg)
    return cfg
