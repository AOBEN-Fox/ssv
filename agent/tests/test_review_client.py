from __future__ import annotations

import json
from pathlib import Path

from ssv_agent.review_client import MockVisionReviewClient, RightCodesVisionReviewClient, parse_review_response
from ssv_agent.review_context import ReviewContext
from ssv_agent.review_prompt import build_review_prompt


def make_context(tmp_path: Path) -> ReviewContext:
    frame = tmp_path / "frame.jpg"
    frame.write_bytes(b"fake-jpeg")
    return ReviewContext.from_event(
        {
            "type": "helmet_violation",
            "event_id": "camera-1:42:1700000000000",
            "source": "camera-1",
            "timestamp_ms": 1700000000000,
            "frame_id": 42,
            "trigger_reason": "head_detected",
            "detections": [{"class": "head", "confidence": 0.86, "bbox": [0.1, 0.2, 0.3, 0.4], "track_id": 7}],
            "evidence": {"frame_path": str(frame), "missing_reason": None},
        },
        provider="mock",
        model="mock-vision",
    )


def test_build_review_prompt_contains_event_and_detection(tmp_path: Path) -> None:
    prompt = build_review_prompt(make_context(tmp_path))

    assert "工地安全视频复核助手" in prompt.system
    assert "event_id: camera-1:42:1700000000000" in prompt.user_text
    assert "class=head" in prompt.user_text
    assert prompt.image_data_url.startswith("data:image/jpeg;base64,")


def test_mock_provider_returns_structured_result(tmp_path: Path) -> None:
    result = MockVisionReviewClient().review(make_context(tmp_path))

    assert result.final_decision == "violation_confirmed"
    assert result.provider == "mock"


def test_parse_review_response_accepts_valid_json() -> None:
    payload = json.dumps(
        {
            "final_decision": "manual_review",
            "confidence": 0.2,
            "reasoning_summary": "看不清。",
            "evidence_description": "画面模糊。",
            "recommended_action": "人工复核。",
        }
    )

    result = parse_review_response(
        event_id="e1",
        content=payload,
        provider="right_codes",
        model="gpt-5.5",
    )

    assert result.final_decision == "manual_review"
    assert result.error is None


def test_right_codes_provider_missing_api_key_returns_manual_review(tmp_path: Path, monkeypatch) -> None:
    monkeypatch.delenv("RIGHT_CODES_API_KEY", raising=False)
    client = RightCodesVisionReviewClient(
        base_url="https://right.codes/codex/v1",
        model="gpt-5.5",
        api_key_env="RIGHT_CODES_API_KEY",
        timeout_seconds=1,
    )

    result = client.review(make_context(tmp_path))

    assert result.final_decision == "manual_review"
    assert result.error == "missing_api_key"
