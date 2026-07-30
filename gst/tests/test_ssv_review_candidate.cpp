#include "ssv_review_candidate.hpp"

#include <cassert>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace {

SsvTrackedObject make_object(
    const char *class_name,
    int track_id,
    SsvTrackState track_state,
    float confidence,
    float x1,
    float y1,
    float x2,
    float y2)
{
    SsvTrackedObject object;
    std::snprintf(
        object.detection.class_name,
        sizeof(object.detection.class_name),
        "%s",
        class_name);
    object.detection.confidence = confidence;
    object.detection.x1 = x1;
    object.detection.y1 = y1;
    object.detection.x2 = x2;
    object.detection.y2 = y2;
    object.track_id = track_id;
    object.track_state = track_state;
    return object;
}

} // namespace

int main()
{
    assert(ssv_review_event_id("camera-01", 3, 12) ==
           "4816f729-74d2-5eba-8f2d-3f2ad285f53e");
    assert(ssv_review_canonical_name("camera-01", 3, 12) ==
           "ssv://review/camera-01/3/12/head_without_helmet_single_frame/1");
    assert(ssv_review_event_dir_name("camera-01", 3, 12, 1785400934927LL) ==
           "20260730T164214.927+0800_camera-01_g3_t12_4816f729");

    const auto head = make_object(
        "head", 12, SSV_TRACK_NEW, 0.91F, 0.20F, 0.10F, 0.38F, 0.32F);
    assert(ssv_review_object_is_eligible(head));
    assert(!ssv_review_object_is_eligible(make_object(
        "helmet", 12, SSV_TRACK_NEW, 0.91F, 0.20F, 0.10F, 0.38F, 0.32F)));
    assert(!ssv_review_object_is_eligible(make_object(
        "head", -1, SSV_TRACK_NEW, 0.91F, 0.20F, 0.10F, 0.38F, 0.32F)));
    assert(!ssv_review_object_is_eligible(make_object(
        "head", 12, SSV_TRACK_LOST, 0.91F, 0.20F, 0.10F, 0.38F, 0.32F)));

    SsvReviewDeduplicator deduplicator;
    deduplicator.reset_for_generation(3);
    const auto key = ssv_review_dedup_key("camera-01", 3, 12);
    assert(!deduplicator.already_published(key));
    deduplicator.mark_published(key);
    assert(deduplicator.already_published(key));
    deduplicator.reset_for_generation(4);
    assert(!deduplicator.already_published(ssv_review_dedup_key("camera-01", 4, 12)));

    SsvTrackedFrame frame;
    frame.frame_id = 128;
    frame.source_id = "camera-01";
    frame.timing = {6200000000ULL, GST_SECOND / 5, 3};
    const auto candidate = ssv_review_make_candidate(
        frame,
        head,
        1785400000000LL,
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
        640,
        480);
    assert(candidate.has_value());
    const auto payload = nlohmann::json::parse(ssv_review_candidate_json(*candidate));
    assert(payload.size() == 18);
    assert(payload["schema_version"] == 1);
    assert(payload["evidence_path"] ==
           "20260730T162640.000+0800_camera-01_g3_t12_4816f729/evidence.jpg");
    assert(payload["pipeline_generation"] == 3);
    assert(payload["media_pts_ns"] == frame.timing.pts);
    assert(payload["candidate_class"] == "head");

    const auto temp_root = std::filesystem::temp_directory_path() / "ssv-review-candidate-test";
    std::filesystem::create_directories(temp_root);
    const SsvFrameEvidence evidence {
        .jpeg_bytes = {0xff, 0xd8, 0xff, 0xd9},
        .sha256 = candidate->evidence_sha256,
        .width = 640,
        .height = 480,
    };
    SsvReviewDeduplicator publish_deduplicator;
    publish_deduplicator.reset_for_generation(3);
    int calls = 0;
    std::string error;
    const auto fail_publish = [&](std::string_view stream, std::string_view candidate_payload) {
        ++calls;
        assert(stream == "ssv:review-candidates");
        assert(nlohmann::json::parse(candidate_payload)["event_id"] == candidate->event_id);
        return false;
    };
    assert(ssv_review_try_publish(
               temp_root, "ssv:review-candidates", *candidate, evidence,
               publish_deduplicator, fail_publish, &error) == SsvReviewPublishResult::Failed);
    assert(!publish_deduplicator.already_published(key));

    const auto ok_publish = [&](std::string_view, std::string_view) {
        ++calls;
        return true;
    };
    assert(ssv_review_try_publish(
               temp_root, "ssv:review-candidates", *candidate, evidence,
               publish_deduplicator, ok_publish, &error) == SsvReviewPublishResult::Published);
    assert(ssv_review_try_publish(
               temp_root, "ssv:review-candidates", *candidate, evidence,
               publish_deduplicator, ok_publish, &error) == SsvReviewPublishResult::SkippedDuplicate);
    assert(calls == 2);
    return 0;
}
