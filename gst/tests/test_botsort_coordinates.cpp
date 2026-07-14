#include "../ssv-track/botsort/botsort_coordinates.hpp"

#include <gst/check/gstcheck.h>

GST_START_TEST(test_coordinate_adapter_round_trips_normalized_bbox) {
    botsort::Detection det;
    det.x1 = 0.125F;
    det.y1 = 0.25F;
    det.x2 = 0.5F;
    det.y2 = 0.75F;

    const auto pixels = botsort::to_pixel_detection(det, 640, 480);
    fail_unless(pixels.x1 == 80.0F);
    fail_unless(pixels.y1 == 120.0F);
    fail_unless(pixels.x2 == 320.0F);
    fail_unless(pixels.y2 == 360.0F);

    const auto normalized = botsort::to_normalized_detection(pixels, 640, 480);
    fail_unless(normalized.x1 == det.x1);
    fail_unless(normalized.y1 == det.y1);
    fail_unless(normalized.x2 == det.x2);
    fail_unless(normalized.y2 == det.y2);
}
GST_END_TEST

void add_botsort_coordinate_tests(TCase *tc) {
    tcase_add_test(tc, test_coordinate_adapter_round_trips_normalized_bbox);
}
