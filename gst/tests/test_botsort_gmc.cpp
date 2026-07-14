#include "../ssv-track/botsort/botsort_gmc.hpp"

#include <gst/check/gstcheck.h>

namespace {

GST_START_TEST(test_gmc_returns_identity_without_frame) {
    botsort::BoTSortGmc gmc(botsort::GmcMethod::kSparseOptFlow, 2);
    auto warp = gmc.estimate(nullptr);
    fail_unless(warp.is_identity());
    fail_unless(gmc.used_fallback_identity());
}
GST_END_TEST

GST_START_TEST(test_gmc_none_mode_skips_fallback_flag) {
    botsort::BoTSortGmc gmc(botsort::GmcMethod::kNone, 2);
    auto warp = gmc.estimate(nullptr);
    fail_unless(warp.is_identity());
    fail_if(gmc.used_fallback_identity());
}
GST_END_TEST

GST_START_TEST(test_sparse_opt_flow_attempt_depends_only_on_previous_points) {
    fail_if(botsort::should_attempt_sparse_opt_flow(4));
    fail_unless(botsort::should_attempt_sparse_opt_flow(5));
}
GST_END_TEST

GST_START_TEST(test_gmc_apply_bbox_translation) {
    botsort::GmcWarp warp;
    warp.m02 = 0.1;
    warp.m12 = 0.2;

    const std::array<float, 4> bbox = {0.1F, 0.2F, 0.3F, 0.4F};
    const auto transformed = botsort::BoTSortGmc::apply_bbox(warp, bbox);
    fail_unless(transformed[0] == 0.2F);
    fail_unless(transformed[1] == 0.4F);
    fail_unless(transformed[2] == 0.4F);
    fail_unless(transformed[3] == 0.6F);
}
GST_END_TEST

}  // namespace

void add_botsort_gmc_tests(TCase *tc) {
    tcase_add_test(tc, test_gmc_returns_identity_without_frame);
    tcase_add_test(tc, test_gmc_none_mode_skips_fallback_flag);
    tcase_add_test(tc, test_sparse_opt_flow_attempt_depends_only_on_previous_points);
    tcase_add_test(tc, test_gmc_apply_bbox_translation);
}
