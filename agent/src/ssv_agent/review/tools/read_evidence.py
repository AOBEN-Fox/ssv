from __future__ import annotations

import hashlib
from io import BytesIO
from pathlib import Path, PurePosixPath

from PIL import Image, UnidentifiedImageError

from ssv_agent.review.contracts import EvidenceBundle, ReviewCandidate, evidence_dir_name


class EvidenceUnavailableError(RuntimeError):
    pass


def read_evidence(candidate: ReviewCandidate, events_root: Path) -> EvidenceBundle:
    try:
        event_dir_name = evidence_dir_name(candidate.evidence_path)
    except ValueError as exc:
        raise EvidenceUnavailableError("evidence_path 非法") from exc
    relative = PurePosixPath(candidate.evidence_path)
    expected = PurePosixPath(event_dir_name) / "evidence.jpg"
    if relative.is_absolute() or ".." in relative.parts or relative != expected:
        raise EvidenceUnavailableError("evidence_path 非法")
    root = events_root.resolve()
    try:
        path = (root / Path(*relative.parts)).resolve(strict=True)
    except OSError as exc:
        raise EvidenceUnavailableError("证据文件不存在或不可访问") from exc
    if not path.is_relative_to(root) or path.parent.name != event_dir_name:
        raise EvidenceUnavailableError("evidence_path 越界")
    try:
        data = path.read_bytes()
    except OSError as exc:
        raise EvidenceUnavailableError("证据文件读取失败") from exc
    if hashlib.sha256(data).hexdigest() != candidate.evidence_sha256:
        raise EvidenceUnavailableError("evidence_sha256 不一致")
    try:
        with Image.open(BytesIO(data)) as image:
            image.verify()
        with Image.open(BytesIO(data)) as image:
            if image.format != "JPEG" or image.size != (
                candidate.evidence_width, candidate.evidence_height
            ):
                raise EvidenceUnavailableError("JPEG 元数据不一致")
            width, height = image.size
    except EvidenceUnavailableError:
        raise
    except (OSError, UnidentifiedImageError) as exc:
        raise EvidenceUnavailableError("JPEG 不可解码") from exc
    return EvidenceBundle(candidate, data, "image/jpeg", width, height)
