from __future__ import annotations

import structlog
from redis import Redis

from ssv_agent.review.contracts import ReviewResult

logger = structlog.get_logger()


def publish_review_result_best_effort(result: ReviewResult, redis_client: Redis, result_stream: str) -> None:
    try:
        redis_client.xadd(result_stream, {"event": result.model_dump_json()})
    except Exception as exc:
        logger.warning("复验结果 Stream 发布失败", event_id=str(result.event_id), error=str(exc))


def save_review_result(
    result: ReviewResult, events_root, event_dir_name: str, redis_client: Redis,
    result_stream: str,
) -> bool:
    event_dir = events_root.resolve() / event_dir_name
    final_path = event_dir / "review-result.json"
    temp_path = event_dir / ".review-result.json.tmp"
    try:
        event_dir.mkdir(parents=True, exist_ok=True)
        temp_path.write_text(result.model_dump_json(indent=2) + "\n", encoding="utf-8")
        temp_path.replace(final_path)
    except OSError as exc:
        temp_path.unlink(missing_ok=True)
        logger.error("复验结果文件写入失败", event_id=str(result.event_id), error=str(exc))
        return False
    publish_review_result_best_effort(result, redis_client, result_stream)
    return True
