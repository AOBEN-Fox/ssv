from __future__ import annotations

from typing import Any

from pathlib import Path

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


class FakeProcessor:
    def __init__(self, result: bool = True) -> None:
        self.result = result
        self.calls: list[Any] = []

    def process(self, candidate: Any) -> bool:
        self.calls.append(candidate)
        return self.result


def make_consumer(monkeypatch: Any) -> tuple[EventConsumer, FakeRedis, FakeProcessor]:
    fake = FakeRedis()
    monkeypatch.setattr("ssv_agent.event_consumer.Redis", lambda **_kwargs: fake)
    processor = FakeProcessor()
    consumer = EventConsumer(SsvConfig(), processor)
    return consumer, fake, processor


def test_ensure_group_creates_stream_group(monkeypatch: Any) -> None:
    consumer, fake, _ = make_consumer(monkeypatch)

    consumer._ensure_group()

    assert fake.created == [("ssv:review-candidates", "ssv-agent", "0", True)]


def test_handle_event_parses_detection_and_acks(monkeypatch: Any) -> None:
    consumer, fake, processor = make_consumer(monkeypatch)
    payload = (Path(__file__).parent / "review/fixtures/review-candidate-v1.json").read_text()

    consumer._handle_event("123-0", {"event": payload})

    assert len(processor.calls) == 1
    assert fake.acked == [("ssv:review-candidates", "ssv-agent", "123-0")]


def test_handle_event_rejects_malformed_json_without_ack(monkeypatch: Any) -> None:
    consumer, fake, _ = make_consumer(monkeypatch)

    consumer._handle_event("123-0", {"event": "{"})

    assert fake.acked == []
