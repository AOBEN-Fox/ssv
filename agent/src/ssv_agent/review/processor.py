from __future__ import annotations

import time
from dataclasses import dataclass
from enum import Enum
from pathlib import Path
from typing import Callable

from pydantic import ValidationError
from redis import Redis

from ssv_agent.config import SsvConfig
from ssv_agent.review.contracts import ReviewCandidate, ReviewDecision, ReviewResult, evidence_dir_name
from ssv_agent.review.tools.read_evidence import EvidenceUnavailableError, read_evidence
from ssv_agent.review.tools.review_vision import review_vision
from ssv_agent.review.tools.save_review_result import publish_review_result_best_effort, save_review_result


@dataclass(frozen=True)
class ReviewModelRuntime:
    model: object
    provider: str
    model_name: str


class ExistingResult(Enum):
    MISSING = "missing"
    VALID = "valid"
    CORRUPT = "corrupt"
    MISMATCHED = "mismatched"


def candidate_identity(candidate: ReviewCandidate) -> dict[str, object]:
    return {key: getattr(candidate, key) for key in ("event_id", "source", "track_id", "rule_id", "rule_version")}


def evidence_unavailable_decision() -> ReviewDecision:
    return ReviewDecision(decision="needs_human_review", review_confidence=0.0,
        primary_reason_code="evidence_unavailable", evidence_summary="单帧证据不存在、越界、损坏或校验失败。",
        recommended_action="检查证据归档后由人工复验。")


def load_existing_result(candidate: ReviewCandidate, events_root: Path) -> tuple[ExistingResult, ReviewResult | None]:
    path = events_root.resolve() / evidence_dir_name(candidate.evidence_path) / "review-result.json"
    if not path.exists():
        return ExistingResult.MISSING, None
    try:
        result = ReviewResult.model_validate_json(path.read_text(encoding="utf-8"))
    except (OSError, ValidationError):
        return ExistingResult.CORRUPT, None
    expected = candidate_identity(candidate)
    actual = {key: getattr(result, key) for key in expected}
    if actual != expected:
        return ExistingResult.MISMATCHED, result
    return ExistingResult.VALID, result


class ReviewProcessor:
    def __init__(self, config: SsvConfig, runtime: ReviewModelRuntime, redis_client: Redis, clock_ms: Callable[[], int] = lambda: time.time_ns() // 1_000_000) -> None:
        self._config, self._runtime, self._redis, self._clock_ms = config, runtime, redis_client, clock_ms

    def process(self, candidate: ReviewCandidate) -> bool:
        root = self._config.artifacts.events_root
        status, existing = load_existing_result(candidate, root)
        if status is ExistingResult.VALID:
            publish_review_result_best_effort(existing, self._redis, self._config.redis.review_result_stream)
            return True
        if status is not ExistingResult.MISSING:
            return False
        try:
            decision = review_vision(read_evidence(candidate, root), self._runtime.model)  # type: ignore[arg-type]
        except EvidenceUnavailableError:
            decision = evidence_unavailable_decision()
        result = ReviewResult(**candidate_identity(candidate), **decision.model_dump(), provider=self._runtime.provider, model=self._runtime.model_name, completed_at_ms=self._clock_ms())
        return save_review_result(
            result, root, evidence_dir_name(candidate.evidence_path), self._redis,
            self._config.redis.review_result_stream,
        )
