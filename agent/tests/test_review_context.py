from __future__ import annotations

import pytest

from ssv_agent.review_context import ReviewContext, ReviewEventError


def test_review_context_parses_helmet_violation_event() -> None:
    event = {
        "type": "helmet_violation",
        "event_id": "camera-1:42:1700000000000",
        "source": "camera-1",
        "timestamp_ms": 1700000000000,
        "frame_id": 42,
        "trigger_reason": "head_detected",
        "detections": [{"class": "head", "confidence": 0.86, "bbox": [0.1, 0.2, 0.3, 0.4], "track_id": 7}],
        "evidence": {"frame_path": "frame.jpg", "missing_reason": None},
    }

    ctx = ReviewContext.from_event(event, provider="mock", model="mock-vision")

    assert ctx.event_id == "camera-1:42:1700000000000"
    assert ctx.frame_path == "frame.jpg"
    assert ctx.detections[0].class_name == "head"
    assert ctx.detections[0].bbox == [0.1, 0.2, 0.3, 0.4]


def test_review_context_rejects_non_violation_event() -> None:
    with pytest.raises(ReviewEventError):
        ReviewContext.from_event({"type": "detection"}, provider="mock", model="mock-vision")
