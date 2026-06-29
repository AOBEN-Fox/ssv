from __future__ import annotations

import json
from pathlib import Path
from typing import Any

from ssv_agent.config import SsvConfig
from ssv_agent.event_consumer import EventConsumer


class FakeRedis:
    def __init__(self, *args: Any, **kwargs: Any) -> None:
        self.acked: list[tuple[str, str, str]] = []
        self.created: list[tuple[str, str, str, bool]] = []

    def xgroup_create(self, stream: str, group: str, id: str, mkstream: bool) -> None:
        self.created.append((stream, group, id, mkstream))

    def xack(self, stream: str, group: str, msg_id: str) -> None:
        self.acked.append((stream, group, msg_id))


def make_consumer(monkeypatch: Any) -> tuple[EventConsumer, FakeRedis]:
    fake = FakeRedis()
    monkeypatch.setattr("ssv_agent.event_consumer.Redis", lambda **_kwargs: fake)
    consumer = EventConsumer(SsvConfig())
    return consumer, fake


def test_ensure_group_creates_stream_group(monkeypatch: Any) -> None:
    consumer, fake = make_consumer(monkeypatch)

    consumer._ensure_group()

    assert fake.created == [("ssv:events", "ssv-agent", "0", True)]


def test_handle_event_parses_detection_and_acks(monkeypatch: Any) -> None:
    consumer, fake = make_consumer(monkeypatch)
    payload = {
        "source": "camera-1",
        "frame_id": 42,
        "detections": [
            {"class": "person", "confidence": 0.91, "track_id": 5},
        ],
    }

    consumer._handle_event("123-0", {"event": json.dumps(payload)})

    assert fake.acked == [("ssv:events", "ssv-agent", "123-0")]


def test_handle_helmet_violation_writes_review_and_acks(tmp_path: Path, monkeypatch: Any) -> None:
    consumer, fake = make_consumer(monkeypatch)
    frame = tmp_path / "frame.jpg"
    frame.write_bytes(b"fake-jpeg")
    consumer._config.reviews.output_dir = str(tmp_path / "reviews")
    consumer._config.agent.model.provider = "mock"
    payload = {
        "type": "helmet_violation",
        "event_id": "camera-1:42:1700000000000",
        "source": "camera-1",
        "timestamp_ms": 1700000000000,
        "frame_id": 42,
        "trigger_reason": "head_detected",
        "detections": [{"class": "head", "confidence": 0.86, "bbox": [0.1, 0.2, 0.3, 0.4], "track_id": 7}],
        "evidence": {"frame_path": str(frame), "missing_reason": None},
    }

    consumer._handle_event("123-0", {"event": json.dumps(payload)})

    assert fake.acked == [("ssv:events", "ssv-agent", "123-0")]
    assert (tmp_path / "reviews" / "camera-1_42_1700000000000.md").exists()


def test_handle_helmet_violation_resolves_relative_paths_from_repo_root(
    tmp_path: Path, monkeypatch: Any
) -> None:
    consumer, fake = make_consumer(monkeypatch)
    repo_root = Path(__file__).resolve().parents[2]
    rel_frame = Path("artifacts/evidence/relative/test.jpg")
    abs_frame = repo_root / rel_frame
    abs_frame.parent.mkdir(parents=True, exist_ok=True)
    abs_frame.write_bytes(b"fake-jpeg")

    rel_reviews = Path("artifacts/reviews-relative")
    abs_reviews = repo_root / rel_reviews
    if abs_reviews.exists():
        for child in abs_reviews.iterdir():
            child.unlink()
    else:
        abs_reviews.mkdir(parents=True)

    consumer._config.reviews.output_dir = str(rel_reviews)
    consumer._config.agent.model.provider = "mock"
    payload = {
        "type": "helmet_violation",
        "event_id": "camera-1:99:1700000000000",
        "source": "camera-1",
        "timestamp_ms": 1700000000000,
        "frame_id": 99,
        "trigger_reason": "head_detected",
        "detections": [{"class": "head", "confidence": 0.86, "bbox": [0.1, 0.2, 0.3, 0.4], "track_id": 7}],
        "evidence": {"frame_path": str(rel_frame), "missing_reason": None},
    }

    consumer._handle_event("123-1", {"event": json.dumps(payload)})

    assert fake.acked == [("ssv:events", "ssv-agent", "123-1")]
    assert (abs_reviews / "camera-1_99_1700000000000.md").exists()


def test_handle_event_rejects_malformed_json_without_ack(monkeypatch: Any) -> None:
    consumer, fake = make_consumer(monkeypatch)

    consumer._handle_event("123-0", {"event": "{"})

    assert fake.acked == []
