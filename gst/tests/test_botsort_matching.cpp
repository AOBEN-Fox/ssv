#include "../ssv-track/botsort/botsort_matching.hpp"

#include <gst/check/gstcheck.h>

#include <vector>

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

GST_START_TEST(test_iou_cost_and_class_compatibility) {
    auto a = make_detection(0, 0.1F, 0.1F, 0.4F, 0.4F, 0.9F, 1);
    auto b = make_detection(1, 0.2F, 0.2F, 0.5F, 0.5F, 0.8F, 1);
    auto c = make_detection(2, 0.2F, 0.2F, 0.5F, 0.5F, 0.8F, 2);

    const float cost = botsort::iou_cost(a, b);
    fail_unless(cost > 0.0F && cost < 1.0F);
    fail_unless(botsort::classes_compatible(a.class_id, b.class_id));
    fail_if(botsort::classes_compatible(a.class_id, c.class_id));
    fail_unless(botsort::fuse_score(cost, b.score) >= cost);
}
GST_END_TEST

GST_START_TEST(test_linear_assignment_and_unmatched_rows) {
    std::vector<std::vector<float>> cost = {
        {0.1F, 0.8F},
        {0.7F, 0.2F},
    };
    auto result = botsort::linear_assignment(cost, 0.5F);
    fail_unless(result.matches.size() == 2);
    fail_unless(result.unmatched_rows.empty());
    fail_unless(result.unmatched_cols.empty());
}
GST_END_TEST

GST_START_TEST(test_restore_input_order_places_detections_by_input_index) {
    std::vector<botsort::Detection> tracked;
    auto first = make_detection(1, 0.2F, 0.2F, 0.4F, 0.4F, 0.9F, 1);
    first.track_id = 22;
    auto second = make_detection(0, 0.1F, 0.1F, 0.3F, 0.3F, 0.8F, 1);
    second.track_id = 11;
    tracked.push_back(first);
    tracked.push_back(second);

    auto ordered = botsort::restore_input_order(tracked, 2);
    fail_unless(ordered[0].track_id == 11);
    fail_unless(ordered[1].track_id == 22);
}
GST_END_TEST

}  // namespace

void add_botsort_matching_tests(TCase *tc) {
    tcase_add_test(tc, test_iou_cost_and_class_compatibility);
    tcase_add_test(tc, test_linear_assignment_and_unmatched_rows);
    tcase_add_test(tc, test_restore_input_order_places_detections_by_input_index);
}
