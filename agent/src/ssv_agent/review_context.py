from __future__ import annotations

from pydantic import BaseModel, Field


class ReviewEventError(ValueError):
    pass


class ReviewDetection(BaseModel):
    class_name: str = Field(alias="class")
    confidence: float
    bbox: list[float]
    track_id: int = -1


class ReviewContext(BaseModel):
    event_id: str
    source: str
    timestamp_ms: int
    frame_id: int
    trigger_reason: str
    detections: list[ReviewDetection]
    frame_path: str | None
    missing_reason: str | None = None
    provider: str
    model: str

    @classmethod
    def from_event(cls, event: dict, provider: str, model: str) -> "ReviewContext":
        if event.get("type") != "helmet_violation":
            raise ReviewEventError("not a helmet_violation event")
        evidence = event.get("evidence") or {}
        return cls.model_validate(
            {
                "event_id": event["event_id"],
                "source": event.get("source", "unknown"),
                "timestamp_ms": event["timestamp_ms"],
                "frame_id": event["frame_id"],
                "trigger_reason": event.get("trigger_reason", "head_detected"),
                "detections": event.get("detections", []),
                "frame_path": evidence.get("frame_path"),
                "missing_reason": evidence.get("missing_reason"),
                "provider": provider,
                "model": model,
            }
        )


class ReviewResult(BaseModel):
    event_id: str
    final_decision: str
    confidence: float
    reasoning_summary: str
    evidence_description: str
    recommended_action: str
    provider: str
    model: str
    error: str | None = None
