#include "../ssv-track/botsort/botsort_processor.hpp"

#include <gst/check/gstcheck.h>

#include <cstdio>
#include <cstring>

namespace {
SsvDetection make_detection(float x1, float y1, float x2, float y2) {
    SsvDetection det{};
    det.x1 = x1; det.y1 = y1; det.x2 = x2; det.y2 = y2;
    det.confidence = 0.95F; det.class_id = 1;
    std::snprintf(det.class_name, sizeof(det.class_name), "person");
    return det;
}
botsort::TrackerConfig make_config() {
    botsort::TrackerConfig config;
    config.gmc_method = botsort::GmcMethod::kNone;
    config.enable_class_constraint = false;
    return config;
}
GST_START_TEST(test_processor_writes_tracking_fields_without_mutating_detection_contract) {
    botsort::BoTSortProcessor processor(make_config());
    std::vector<SsvDetection> first{make_detection(0.10F, 0.10F, 0.30F, 0.30F)};
    const SsvDetection first_before = first[0];
    processor.process(first, 640, 480);
    fail_unless(first[0].track_id > 0);
    fail_unless(first[0].track_state == SSV_TRACK_NEW);
    fail_unless(!first[0].occluded);
    fail_unless(first[0].x1 == first_before.x1 && first[0].y1 == first_before.y1);
    fail_unless(first[0].x2 == first_before.x2 && first[0].y2 == first_before.y2);
    fail_unless(first[0].confidence == first_before.confidence);
    fail_unless(first[0].class_id == first_before.class_id);
    fail_unless(std::strcmp(first[0].class_name, first_before.class_name) == 0);
    std::vector<SsvDetection> second{make_detection(0.11F, 0.10F, 0.31F, 0.30F)};
    const SsvDetection second_before = second[0];
    processor.process(second, 640, 480);
    fail_unless(second[0].track_id == first[0].track_id);
    fail_unless(second[0].track_state == SSV_TRACK_MATCHED);
    fail_unless(second[0].x1 == second_before.x1 && second[0].y1 == second_before.y1);
    fail_unless(second[0].x2 == second_before.x2 && second[0].y2 == second_before.y2);
    fail_unless(second[0].confidence == second_before.confidence);
    fail_unless(second[0].class_id == second_before.class_id);
    fail_unless(std::strcmp(second[0].class_name, second_before.class_name) == 0);
}
GST_END_TEST
}
void add_botsort_processor_tests(TCase *tc) {
    tcase_add_test(tc, test_processor_writes_tracking_fields_without_mutating_detection_contract);
}
