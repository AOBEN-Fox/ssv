from __future__ import annotations

import json
from dataclasses import dataclass
from pathlib import Path

from ssv_agent.review_context import ReviewResult


@dataclass(frozen=True)
class ReviewOutputPaths:
    json_path: Path
    markdown_path: Path


def safe_event_id(event_id: str) -> str:
    return "".join(ch if ch.isalnum() or ch in "._-" else "_" for ch in event_id) or "unknown"


def write_review_outputs(
    output_dir: str | Path,
    result: ReviewResult,
    source: str,
    frame_id: int,
    trigger_reason: str,
    frame_path: str | None,
) -> ReviewOutputPaths:
    out_dir = Path(output_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    stem = safe_event_id(result.event_id)
    json_path = out_dir / f"{stem}.json"
    markdown_path = out_dir / f"{stem}.md"

    json_path.write_text(
        json.dumps(result.model_dump(), ensure_ascii=False, indent=2),
        encoding="utf-8",
    )
    markdown_path.write_text(
        "\n".join(
            [
                f"# 安全帽复核结果 {result.event_id}",
                "",
                f"- 视频源: {source}",
                f"- 帧 ID: {frame_id}",
                f"- 最终判断: {result.final_decision}",
                f"- 置信度: {result.confidence:.2f}",
                f"- 触发原因: {trigger_reason}",
                f"- 证据图片: {frame_path or '无'}",
                f"- 复核说明: {result.reasoning_summary}",
                f"- 画面描述: {result.evidence_description}",
                f"- 建议动作: {result.recommended_action}",
                f"- Provider: {result.provider}",
                f"- Model: {result.model}",
                f"- Error: {result.error or '无'}",
                "",
            ]
        ),
        encoding="utf-8",
    )
    return ReviewOutputPaths(json_path=json_path, markdown_path=markdown_path)
