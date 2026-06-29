from __future__ import annotations

import base64
from dataclasses import dataclass
from pathlib import Path

from ssv_agent.review_context import ReviewContext


@dataclass(frozen=True)
class ReviewPrompt:
    system: str
    user_text: str
    image_data_url: str


SYSTEM_PROMPT = (
    "你是工地安全视频复核助手。你只能基于提供的图片证据和检测元数据判断，"
    "不得编造图片外信息。任务是确认触发目标是否未佩戴安全帽。"
    "如果图片看不清、目标被遮挡、目标不完整、检测框明显不可信或证据不足，"
    "必须返回 manual_review。输出必须是严格 JSON，不要输出 Markdown。"
)


def build_review_prompt(ctx: ReviewContext) -> ReviewPrompt:
    if ctx.frame_path is None:
        raise FileNotFoundError("frame_path is missing")
    image_bytes = Path(ctx.frame_path).read_bytes()
    image_data_url = "data:image/jpeg;base64," + base64.b64encode(image_bytes).decode("ascii")
    lines = [
        "请复核以下安全帽违规事件。",
        "",
        f"event_id: {ctx.event_id}",
        f"source: {ctx.source}",
        f"frame_id: {ctx.frame_id}",
        f"trigger_reason: {ctx.trigger_reason}",
        "detections:",
    ]
    for det in ctx.detections:
        lines.append(
            f"- class={det.class_name}, confidence={det.confidence:.3f}, "
            f"bbox={det.bbox}, track_id={det.track_id}"
        )
    lines.extend(
        [
            "",
            "请判断触发目标是否未佩戴安全帽。只输出 JSON，字段包括 "
            "final_decision, confidence, reasoning_summary, evidence_description, recommended_action。",
        ]
    )
    return ReviewPrompt(system=SYSTEM_PROMPT, user_text="\n".join(lines), image_data_url=image_data_url)
