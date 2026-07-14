#include "../ssv-track/botsort/botsort_kalman.hpp"
#include "../ssv-track/botsort/botsort_types.hpp"

#include <gst/check/gstcheck.h>

#include <vector>

namespace {


GST_START_TEST(test_kalman_initiate_predict_update) {
    botsort::BoTSortKalman kalman;
    auto state = kalman.initiate(0.5F, 0.5F, 0.2F, 0.4F);
    auto predicted = kalman.predict(state);
    auto updated = kalman.update(predicted, 0.52F, 0.48F, 0.2F, 0.4F);

    fail_unless(updated.mean[0] >= 0.5F);
    fail_unless(updated.mean[0] <= 0.52F);
    fail_unless(updated.mean[1] >= 0.48F);
    fail_unless(updated.mean[1] <= 0.5F);
}
GST_END_TEST


}  // namespace

void add_botsort_kalman_tests(TCase *tc) {
    tcase_add_test(tc, test_kalman_initiate_predict_update);
}
