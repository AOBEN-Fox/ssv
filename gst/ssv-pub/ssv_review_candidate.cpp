#include "ssv_review_candidate.hpp"

#include <glib.h>
#include <gst/gst.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>

#include <nlohmann/json.hpp>

namespace {

constexpr std::array<std::uint8_t, 16> kUuidUrlNamespace = {
    0x6b, 0xa7, 0xb8, 0x11, 0x9d, 0xad, 0x11, 0xd1,
    0x80, 0xb4, 0x00, 0xc0, 0x4f, 0xd4, 0x30, 0xc8,
};

bool is_lower_hex_sha256(std::string_view value)
{
    if (value.size() != 64) {
        return false;
    }
    return std::all_of(value.begin(), value.end(), [](char character) {
        return (character >= '0' && character <= '9') ||
            (character >= 'a' && character <= 'f');
    });
}

std::string uuid_v5(std::string_view name)
{
    const std::unique_ptr<GChecksum, decltype(&g_checksum_free)> checksum(
        g_checksum_new(G_CHECKSUM_SHA1), g_checksum_free);
    g_checksum_update(checksum.get(), kUuidUrlNamespace.data(), kUuidUrlNamespace.size());
    g_checksum_update(
        checksum.get(), reinterpret_cast<const guchar *>(name.data()), name.size());

    std::array<guint8, 20> digest = {};
    gsize digest_size = digest.size();
    g_checksum_get_digest(checksum.get(), digest.data(), &digest_size);
    digest[6] = static_cast<guint8>((digest[6] & 0x0fU) | 0x50U);
    digest[8] = static_cast<guint8>((digest[8] & 0x3fU) | 0x80U);

    char value[37] = {};
    std::snprintf(
        value,
        sizeof(value),
        "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        digest[0], digest[1], digest[2], digest[3], digest[4], digest[5], digest[6],
        digest[7], digest[8], digest[9], digest[10], digest[11], digest[12], digest[13],
        digest[14], digest[15]);
    return value;
}

std::string safe_source_component(std::string_view source)
{
    std::string result;
    result.reserve(source.size());
    for (const auto character : source) {
        const bool safe = (character >= 'a' && character <= 'z') ||
            (character >= 'A' && character <= 'Z') ||
            (character >= '0' && character <= '9') || character == '.' ||
            character == '_' || character == '-';
        result.push_back(safe ? character : '_');
    }
    return result.empty() ? "source" : result;
}

} // namespace

std::string ssv_review_canonical_name(
    std::string_view source, std::uint64_t generation, int track_id)
{
    return "ssv://review/" + std::string(source) + "/" +
        std::to_string(generation) + "/" + std::to_string(track_id) + "/" +
        std::string(SSV_REVIEW_RULE_ID) + "/" + std::to_string(SSV_REVIEW_RULE_VERSION);
}

std::string ssv_review_event_id(
    std::string_view source, std::uint64_t generation, int track_id)
{
    return uuid_v5(ssv_review_canonical_name(source, generation, track_id));
}

std::string ssv_review_event_dir_name(
    std::string_view source, std::uint64_t generation, int track_id,
    std::int64_t timestamp_ms)
{
    const auto seconds = timestamp_ms / 1000;
    const auto milliseconds = timestamp_ms % 1000;
    const std::unique_ptr<GDateTime, decltype(&g_date_time_unref)> utc(
        g_date_time_new_from_unix_utc(seconds), g_date_time_unref);
    if (utc == nullptr || milliseconds < 0) {
        return {};
    }
    const std::unique_ptr<GDateTime, decltype(&g_date_time_unref)> beijing(
        g_date_time_add_hours(utc.get(), 8), g_date_time_unref);
    const std::unique_ptr<gchar, decltype(&g_free)> datetime(
        g_date_time_format(beijing.get(), "%Y%m%dT%H%M%S"), g_free);
    const auto event_id = ssv_review_event_id(source, generation, track_id);
    char directory[256] = {};
    std::snprintf(
        directory, sizeof(directory), "%s.%03lld+0800_%s_g%llu_t%d_%.8s",
        datetime.get(), static_cast<long long>(milliseconds),
        safe_source_component(source).c_str(),
        static_cast<unsigned long long>(generation), track_id, event_id.c_str());
    return directory;
}

std::string ssv_review_dedup_key(
    std::string_view source, std::uint64_t generation, int track_id)
{
    return ssv_review_canonical_name(source, generation, track_id);
}

bool ssv_review_object_is_eligible(const SsvTrackedObject &object)
{
    const auto &detection = object.detection;
    return std::string_view(detection.class_name) == "head" && object.track_id >= 0 &&
        (object.track_state == SSV_TRACK_NEW || object.track_state == SSV_TRACK_MATCHED) &&
        std::isfinite(detection.x1) && std::isfinite(detection.y1) &&
        std::isfinite(detection.x2) && std::isfinite(detection.y2) &&
        0.0F <= detection.x1 && detection.x1 < detection.x2 && detection.x2 <= 1.0F &&
        0.0F <= detection.y1 && detection.y1 < detection.y2 && detection.y2 <= 1.0F;
}

std::optional<SsvReviewCandidate> ssv_review_make_candidate(
    const SsvTrackedFrame &frame,
    const SsvTrackedObject &object,
    std::int64_t timestamp_ms,
    std::string evidence_sha256,
    std::uint32_t evidence_width,
    std::uint32_t evidence_height)
{
    if (frame.source_id.empty() || frame.timing.pts == GST_CLOCK_TIME_NONE ||
        !ssv_review_object_is_eligible(object) || !is_lower_hex_sha256(evidence_sha256) ||
        evidence_width == 0 || evidence_height == 0) {
        return std::nullopt;
    }

    SsvReviewCandidate candidate;
    candidate.event_id = ssv_review_event_id(
        frame.source_id, frame.timing.generation, object.track_id);
    candidate.source = frame.source_id;
    candidate.pipeline_generation = frame.timing.generation;
    candidate.frame_id = frame.frame_id;
    candidate.media_pts_ns = frame.timing.pts;
    candidate.timestamp_ms = timestamp_ms;
    candidate.track_id = object.track_id;
    candidate.detection_confidence = object.detection.confidence;
    candidate.bbox = {
        object.detection.x1,
        object.detection.y1,
        object.detection.x2,
        object.detection.y2,
    };
    const auto event_dir_name = ssv_review_event_dir_name(
        frame.source_id, frame.timing.generation, object.track_id, timestamp_ms);
    if (event_dir_name.empty()) {
        return std::nullopt;
    }
    candidate.evidence_path = event_dir_name + "/evidence.jpg";
    candidate.evidence_sha256 = std::move(evidence_sha256);
    candidate.evidence_width = evidence_width;
    candidate.evidence_height = evidence_height;
    return candidate;
}

std::string ssv_review_candidate_json(const SsvReviewCandidate &candidate)
{
    return nlohmann::json({
        {"type", "review_candidate"},
        {"schema_version", 1},
        {"event_id", candidate.event_id},
        {"source", candidate.source},
        {"pipeline_generation", candidate.pipeline_generation},
        {"frame_id", candidate.frame_id},
        {"media_pts_ns", candidate.media_pts_ns},
        {"timestamp_ms", candidate.timestamp_ms},
        {"track_id", candidate.track_id},
        {"rule_id", SSV_REVIEW_RULE_ID},
        {"rule_version", SSV_REVIEW_RULE_VERSION},
        {"candidate_class", "head"},
        {"detection_confidence", candidate.detection_confidence},
        {"bbox", candidate.bbox},
        {"evidence_path", candidate.evidence_path},
        {"evidence_sha256", candidate.evidence_sha256},
        {"evidence_width", candidate.evidence_width},
        {"evidence_height", candidate.evidence_height},
    }).dump();
}

void SsvReviewDeduplicator::reset_for_generation(std::uint64_t generation)
{
    if (generation_ == generation) {
        return;
    }
    generation_ = generation;
    published_.clear();
}

bool SsvReviewDeduplicator::already_published(std::string_view key) const
{
    return published_.contains(std::string(key));
}

void SsvReviewDeduplicator::mark_published(std::string key)
{
    published_.insert(std::move(key));
}

SsvReviewPublishResult ssv_review_try_publish(
    const std::filesystem::path &events_root,
    std::string_view stream_key,
    const SsvReviewCandidate &candidate,
    const SsvFrameEvidence &evidence,
    SsvReviewDeduplicator &deduplicator,
    const SsvReviewPublishFn &publish,
    std::string *error)
{
    const auto key = ssv_review_dedup_key(
        candidate.source, candidate.pipeline_generation, candidate.track_id);
    if (deduplicator.already_published(key)) {
        return SsvReviewPublishResult::SkippedDuplicate;
    }
    if (events_root.empty() || stream_key.empty() || !publish) {
        if (error != nullptr) {
            *error = "复验事件发布参数无效";
        }
        return SsvReviewPublishResult::Failed;
    }

    const auto event_dir = events_root / std::filesystem::path(candidate.evidence_path).parent_path();
    std::error_code filesystem_error;
    std::filesystem::create_directories(event_dir, filesystem_error);
    if (filesystem_error) {
        if (error != nullptr) {
            *error = "无法创建复验证据目录: " + filesystem_error.message();
        }
        return SsvReviewPublishResult::Failed;
    }
    if (!ssv_write_atomic_bytes(event_dir / "evidence.jpg", evidence.jpeg_bytes, error)) {
        return SsvReviewPublishResult::Failed;
    }

    const auto payload = ssv_review_candidate_json(candidate);
    const auto candidate_bytes = std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t *>(payload.data()), payload.size());
    if (!ssv_write_atomic_bytes(event_dir / "candidate.json", candidate_bytes, error)) {
        return SsvReviewPublishResult::Failed;
    }
    if (!publish(stream_key, payload)) {
        if (error != nullptr) {
            *error = "Redis 复验候选发布失败";
        }
        return SsvReviewPublishResult::Failed;
    }
    deduplicator.mark_published(key);
    return SsvReviewPublishResult::Published;
}
