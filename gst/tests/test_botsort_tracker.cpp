#include "../ssv-track/botsort/botsort_tracker.hpp"

#include <gst/check/gstcheck.h>
#include <cmath>

namespace {

botsort::Detection make_detection(int input_index, float x1, float y1, float x2, float y2, float score, int class_id) {
    botsort::Detection det;
    det.input_index = input_index;
    det.x1 = x1;
    det.y1 = y1;
    det.x2 = x2;
    det.y2 = y2;
    det.score = score;
    det.class_id = class_id;
    det.class_name = "person";
    return det;
}

botsort::TrackerConfig make_config() {
    botsort::TrackerConfig cfg;
    cfg.gmc_method = botsort::GmcMethod::kNone;
    cfg.enable_class_constraint = false;
    return cfg;
}

GST_START_TEST(test_tracker_gmc_transforms_full_kalman_state) {
    botsort::BoTSortKalman kalman;
    const auto state = kalman.initiate(10.0F, 20.0F, 4.0F, 8.0F);
    botsort::GmcWarp warp;
    warp.m02 = 3.0;
    warp.m12 = -2.0;
    const auto transformed = botsort::BoTSortTracker::apply_gmc_to_state(state, warp);
    fail_unless(fabsf(transformed.mean[0] - 13.0F) < 1e-5F);
    fail_unless(fabsf(transformed.mean[1] - 18.0F) < 1e-5F);
    fail_unless(fabsf(transformed.mean[2] - 4.0F) < 1e-5F);
    fail_unless(fabsf(transformed.mean[3] - 8.0F) < 1e-5F);
    fail_unless(fabsf(transformed.covariance[0] - state.covariance[0]) < 1e-5F);
}
GST_END_TEST

GST_START_TEST(test_tracker_matches_python_threshold_boundaries) {
    auto cfg = make_config();
    cfg.track_low_thresh = 0.1F;
    cfg.track_high_thresh = 0.6F;
    cfg.new_track_thresh = 0.7F;
    botsort::BoTSortTracker tracker(cfg);

    auto low_equal = tracker.update({make_detection(0, 0.1F, 0.1F, 0.3F, 0.3F, 0.1F, 1)});
    fail_unless(low_equal.stats.filtered_count == 0);

    auto high_equal = tracker.update({make_detection(0, 0.1F, 0.1F, 0.3F, 0.3F, 0.6F, 1)});
    fail_unless(high_equal.stats.high_count == 0);
    fail_unless(high_equal.stats.low_count == 1);

    auto new_equal = tracker.update({make_detection(0, 0.1F, 0.1F, 0.3F, 0.3F, 0.7F, 1)});
    fail_unless(new_equal.stats.new_tracks == 1);
}
GST_END_TEST

GST_START_TEST(test_tracker_reuses_id_for_consecutive_high_confidence_detections) {
    botsort::BoTSortTracker tracker(make_config());
    auto frame1 = tracker.update({make_detection(0, 0.10F, 0.10F, 0.30F, 0.30F, 0.95F, 1)});
    auto frame2 = tracker.update({make_detection(0, 0.11F, 0.10F, 0.31F, 0.30F, 0.93F, 1)});

    fail_unless(frame1.detections.size() == 1);
    fail_unless(frame2.detections.size() == 1);
    fail_unless(frame1.detections[0].track_id == frame2.detections[0].track_id);
    fail_unless(frame1.detections[0].track_state == botsort::TrackState::kNew);
    fail_unless(frame2.detections[0].track_state == botsort::TrackState::kMatched);
}
GST_END_TEST

GST_START_TEST(test_tracker_outputs_kalman_bbox_after_match) {
    botsort::BoTSortTracker tracker(make_config());
    tracker.update({make_detection(0, 0.10F, 0.10F, 0.30F, 0.30F, 0.95F, 1)});
    auto result = tracker.update({make_detection(0, 0.11F, 0.10F, 0.31F, 0.30F, 0.93F, 1)});
    fail_unless(result.detections.size() == 1);
    fail_unless(result.detections[0].x1 > 0.10F);
    fail_unless(result.detections[0].x1 < 0.11F);
}
GST_END_TEST

GST_START_TEST(test_tracker_recovers_low_score_detection_in_second_stage) {
    botsort::BoTSortTracker tracker(make_config());
    auto frame1 = tracker.update({make_detection(0, 0.10F, 0.10F, 0.30F, 0.30F, 0.95F, 1)});
    auto frame2 = tracker.update({make_detection(0, 0.11F, 0.10F, 0.31F, 0.30F, 0.40F, 1)});

    fail_unless(frame1.detections.size() == 1);
    fail_unless(frame2.detections.size() == 1);
    fail_unless(frame2.detections[0].track_id == frame1.detections[0].track_id);
    fail_unless(frame2.detections[0].track_state == botsort::TrackState::kMatched);
    fail_unless(frame2.detections[0].occluded);
}
GST_END_TEST

GST_START_TEST(test_tracker_reactivates_lost_track_with_same_id) {
    botsort::BoTSortTracker tracker(make_config());
    auto frame1 = tracker.update({make_detection(0, 0.10F, 0.10F, 0.30F, 0.30F, 0.95F, 1)});
    auto frame2 = tracker.update({});
    auto frame3 = tracker.update({make_detection(0, 0.11F, 0.10F, 0.31F, 0.30F, 0.92F, 1)});

    fail_unless(frame1.detections.size() == 1);
    fail_unless(frame2.detections.empty());
    fail_unless(frame3.detections.size() == 1);
    fail_unless(frame3.detections[0].track_id == frame1.detections[0].track_id);
    fail_unless(frame3.detections[0].track_state == botsort::TrackState::kMatched);
}
GST_END_TEST

GST_START_TEST(test_tracker_assigns_incrementing_ids_for_multiple_objects) {
    botsort::BoTSortTracker tracker(make_config());
    auto frame = tracker.update({
        make_detection(0, 0.10F, 0.10F, 0.20F, 0.20F, 0.91F, 1),
        make_detection(1, 0.50F, 0.50F, 0.60F, 0.60F, 0.93F, 1),
    });

    fail_unless(frame.detections.size() == 2);
    fail_unless(frame.detections[0].track_id == 1);
    fail_unless(frame.detections[1].track_id == 2);
}
GST_END_TEST

GST_START_TEST(test_tracker_emits_new_track_after_first_frame) {
    botsort::BoTSortTracker tracker(make_config());
    auto first = tracker.update({make_detection(0, 0.10F, 0.10F, 0.30F, 0.30F, 0.95F, 1)});
    auto second = tracker.update({make_detection(0, 0.70F, 0.70F, 0.90F, 0.90F, 0.95F, 2)});

    fail_unless(first.detections.size() == 1);
    fail_unless(second.detections.size() == 1);
    fail_unless(second.detections[0].track_state == botsort::TrackState::kNew);
    fail_unless(second.detections[0].track_id != first.detections[0].track_id);
}
GST_END_TEST

}  // namespace

void add_botsort_tracker_tests(TCase *tc) {
    tcase_add_test(tc, test_tracker_gmc_transforms_full_kalman_state);
    tcase_add_test(tc, test_tracker_matches_python_threshold_boundaries);
    tcase_add_test(tc, test_tracker_reuses_id_for_consecutive_high_confidence_detections);
    tcase_add_test(tc, test_tracker_outputs_kalman_bbox_after_match);
    tcase_add_test(tc, test_tracker_recovers_low_score_detection_in_second_stage);
    tcase_add_test(tc, test_tracker_reactivates_lost_track_with_same_id);
    tcase_add_test(tc, test_tracker_assigns_incrementing_ids_for_multiple_objects);
    tcase_add_test(tc, test_tracker_emits_new_track_after_first_frame);
}
