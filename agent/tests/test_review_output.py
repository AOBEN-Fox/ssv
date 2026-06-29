from __future__ import annotations

from pathlib import Path

from ssv_agent.review_context import ReviewResult
from ssv_agent.review_output import safe_event_id, write_review_outputs


def test_safe_event_id_replaces_unsafe_characters() -> None:
    assert safe_event_id("camera-1:42:1700000000000") == "camera-1_42_1700000000000"


def test_write_review_outputs_writes_json_and_markdown(tmp_path: Path) -> None:
    result = ReviewResult(
        event_id="camera-1:42:1700000000000",
        final_decision="violation_confirmed",
        confidence=0.84,
        reasoning_summary="未见安全帽覆盖。",
        evidence_description="画面中头部裸露。",
        recommended_action="建议现场提醒。",
        provider="mock",
        model="mock-vision",
        error=None,
    )

    written = write_review_outputs(
        output_dir=tmp_path,
        result=result,
        source="camera-1",
        frame_id=42,
        trigger_reason="head_detected",
        frame_path="frame.jpg",
    )

    assert written.json_path.exists()
    assert written.markdown_path.exists()
    text = written.markdown_path.read_text(encoding="utf-8")
    assert "violation_confirmed" in text
    assert "frame.jpg" in text
