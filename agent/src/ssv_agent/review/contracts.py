from __future__ import annotations

from dataclasses import dataclass
import re
from pathlib import PurePosixPath
from typing import Literal

from pydantic import BaseModel, Field, UUID5, model_validator


Decision = Literal["confirmed_no_helmet", "rejected", "needs_human_review"]
ReasonCode = Literal[
    "no_helmet_visible", "helmet_visible", "low_confidence", "evidence_unavailable",
    "provider_unavailable", "invalid_model_output",
]


def _event_dir_name_is_safe(value: str) -> bool:
    return re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._+-]*", value) is not None


def evidence_dir_name(evidence_path: str) -> str:
    path = PurePosixPath(evidence_path)
    if path.is_absolute() or len(path.parts) != 2 or path.name != "evidence.jpg" or not _event_dir_name_is_safe(path.parent.name):
        raise ValueError("evidence_path 必须指向安全的单层事件目录中的 evidence.jpg")
    return path.parent.name


class ReviewCandidate(BaseModel):
    type: Literal["review_candidate"]
    schema_version: Literal[1]
    event_id: UUID5
    source: str
    pipeline_generation: int = Field(ge=0)
    frame_id: int = Field(ge=0)
    media_pts_ns: int = Field(ge=0)
    timestamp_ms: int = Field(ge=0)
    track_id: int = Field(ge=0)
    rule_id: Literal["head_without_helmet_single_frame"]
    rule_version: Literal[1]
    candidate_class: Literal["head"]
    detection_confidence: float = Field(ge=0.0, le=1.0)
    bbox: tuple[float, float, float, float]
    evidence_path: str
    evidence_sha256: str = Field(pattern=r"^[0-9a-f]{64}$")
    evidence_width: int = Field(gt=0)
    evidence_height: int = Field(gt=0)

    @model_validator(mode="after")
    def validate_identity_and_geometry(self) -> "ReviewCandidate":
        x1, y1, x2, y2 = self.bbox
        if not (0.0 <= x1 < x2 <= 1.0 and 0.0 <= y1 < y2 <= 1.0):
            raise ValueError("bbox 必须是有效归一化坐标")
        evidence_dir_name(self.evidence_path)
        if not self.source.strip():
            raise ValueError("source 不得为空")
        return self


class ReviewDecision(BaseModel):
    decision: Decision
    review_confidence: float = Field(ge=0.0, le=1.0)
    primary_reason_code: ReasonCode
    evidence_summary: str = Field(min_length=1)
    recommended_action: str = Field(min_length=1)

    @model_validator(mode="after")
    def validate_reason_mapping(self) -> "ReviewDecision":
        allowed = {
            "confirmed_no_helmet": {"no_helmet_visible"},
            "rejected": {"helmet_visible"},
            "needs_human_review": {
                "low_confidence", "evidence_unavailable", "provider_unavailable",
                "invalid_model_output",
            },
        }
        if self.primary_reason_code not in allowed[self.decision]:
            raise ValueError("decision 与 primary_reason_code 不匹配")
        return self


class ReviewResult(ReviewDecision):
    type: Literal["review_result"] = "review_result"
    schema_version: Literal[1] = 1
    event_id: UUID5
    source: str
    track_id: int
    rule_id: Literal["head_without_helmet_single_frame"]
    rule_version: Literal[1]
    provider: str
    model: str
    completed_at_ms: int


@dataclass(frozen=True)
class EvidenceBundle:
    candidate: ReviewCandidate
    jpeg_bytes: bytes
    media_type: Literal["image/jpeg"]
    width: int
    height: int
