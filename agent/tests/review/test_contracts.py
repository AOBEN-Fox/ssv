from __future__ import annotations

from pathlib import Path

import pytest
from pydantic import ValidationError

from ssv_agent.review.contracts import ReviewCandidate, ReviewDecision


CANDIDATE_FIXTURE = Path(__file__).parent / "fixtures/review-candidate-v1.json"


def test_candidate_fixture_obeys_v1_contract() -> None:
    candidate = ReviewCandidate.model_validate_json(CANDIDATE_FIXTURE.read_text())
    assert candidate.event_id.version == 5
    assert candidate.candidate_class == "head"
    assert candidate.evidence_path == f"{candidate.event_id}/evidence.jpg"


def test_candidate_uses_human_readable_evidence_directory() -> None:
    candidate = ReviewCandidate.model_validate_json(CANDIDATE_FIXTURE.read_text())
    event_dir_name = "20260730T164214.927+0800_camera-01_g3_t12_4816f729"
    readable = ReviewCandidate.model_validate({
        **candidate.model_dump(mode="json"),
        "evidence_path": f"{event_dir_name}/evidence.jpg",
    })
    assert readable.evidence_path == f"{event_dir_name}/evidence.jpg"


def test_candidate_rejects_unsafe_evidence_directory() -> None:
    candidate = ReviewCandidate.model_validate_json(CANDIDATE_FIXTURE.read_text())
    with pytest.raises(ValidationError, match="evidence_path"):
        ReviewCandidate.model_validate({
            **candidate.model_dump(mode="json"),
            "evidence_path": "camera 01/evidence.jpg",
        })


def test_candidate_rejects_non_head() -> None:
    candidate = ReviewCandidate.model_validate_json(CANDIDATE_FIXTURE.read_text())
    with pytest.raises(ValidationError):
        ReviewCandidate.model_validate({**candidate.model_dump(mode="json"), "candidate_class": "person"})


def test_decision_rejects_conflicting_reason() -> None:
    with pytest.raises(ValidationError):
        ReviewDecision(
            decision="confirmed_no_helmet",
            review_confidence=0.93,
            primary_reason_code="helmet_visible",
            evidence_summary="冲突",
            recommended_action="人工确认",
        )
