from __future__ import annotations

import structlog
from redis import Redis

from ssv_agent.config import SsvConfig
from ssv_agent.event_consumer import run_consumer
from ssv_agent.review.deerflow_adapter import build_review_model
from ssv_agent.review.processor import ReviewProcessor

logger = structlog.get_logger()


def run(config: SsvConfig) -> None:
    """启动安全帽复验候选消费者。"""
    if not config.review.enabled:
        logger.info("安全帽视觉复验未启用", review_enabled=False)
        return
    redis_client = Redis(host=config.redis.host, port=config.redis.port, db=config.redis.db, decode_responses=True)
    runtime = build_review_model(config)
    processor = ReviewProcessor(config, runtime, redis_client)
    logger.info(
        "agent service starting",
        version=config.version,
        redis=f"{config.redis.host}:{config.redis.port}",
        stream=config.redis.review_candidate_stream,
        result_stream=config.redis.review_result_stream,
        model_visible_tools=0,
    )

    run_consumer(config, processor, redis_client)

    logger.info("agent service stopped")
