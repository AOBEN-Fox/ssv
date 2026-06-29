#include "ssv_meta.hpp"

#include <cstdint>
#include <optional>
#include <string>

std::optional<std::string> ssv_pub_build_helmet_violation_payload(
    const SsvFrameDetections &det,
    std::int64_t timestamp_ms,
    const std::string &trigger_class,
    const char *frame_path,
    int frame_width,
    int frame_height,
    const char *missing_reason);

#include <gst/check/gstcheck.h>
#include <gst/gst.h>
#include <gst/app/app.h>
#include <gst/video/video.h>

#include <cstring>

extern void run_ssv_meta_tests();

static SsvDetection make_pub_det(const char *name, int class_id, float confidence, int track_id) {
    SsvDetection d{};
    std::snprintf(d.class_name, sizeof(d.class_name), "%s", name);
    d.class_id = class_id;
    d.confidence = confidence;
    d.x1 = 0.1f;
    d.y1 = 0.2f;
    d.x2 = 0.3f;
    d.y2 = 0.4f;
    d.track_id = track_id;
    d.track_state = SSV_TRACK_MATCHED;
    d.occluded = false;
    return d;
}

static void assert_element_factory(const char *name) {
    GstElement *element = gst_element_factory_make(name, nullptr);
    fail_unless(element != nullptr, "missing element factory: %s", name);
    gst_object_unref(element);
}

GST_START_TEST(test_ssv_plugin_factories_are_registered) {
    assert_element_factory("ssvtemplate");
    assert_element_factory("ssvinfer");
    assert_element_factory("ssvtrack");
    assert_element_factory("ssvpub");
    assert_element_factory("ssvoverlay");
}
GST_END_TEST

GST_START_TEST(test_ssvinfer_exposes_label_map_property) {
    GstElement *element = gst_element_factory_make("ssvinfer", nullptr);
    fail_unless(element != nullptr);

    gchar *target_class = nullptr;
    g_object_get(element, "target-class", &target_class, nullptr);
    fail_unless(target_class != nullptr);
    fail_unless(std::string(target_class).empty());
    g_free(target_class);

    g_object_set(element, "label-map", "config/model-labels/coco80.txt", nullptr);
    gchar *label_map = nullptr;
    g_object_get(element, "label-map", &label_map, nullptr);
    fail_unless(label_map != nullptr);
    fail_unless(std::string(label_map) == "config/model-labels/coco80.txt");

    g_free(label_map);
    gst_object_unref(element);
}
GST_END_TEST

GST_START_TEST(test_ssvinfer_exposes_device_properties) {
    GstElement *element = gst_element_factory_make("ssvinfer", nullptr);
    fail_unless(element != nullptr);

    gchar *device = nullptr;
    gboolean cuda_required = TRUE;
    gint cuda_device_id = -1;
    g_object_get(element,
        "device", &device,
        "cuda-device-id", &cuda_device_id,
        "cuda-required", &cuda_required,
        nullptr);
    fail_unless(device != nullptr);
    fail_unless(std::string(device) == "auto");
    fail_unless(cuda_device_id == 0);
    fail_unless(cuda_required == FALSE);
    g_free(device);

    g_object_set(element,
        "device", "cuda",
        "cuda-device-id", 1,
        "cuda-required", TRUE,
        nullptr);
    g_object_get(element,
        "device", &device,
        "cuda-device-id", &cuda_device_id,
        "cuda-required", &cuda_required,
        nullptr);
    fail_unless(device != nullptr);
    fail_unless(std::string(device) == "cuda");
    fail_unless(cuda_device_id == 1);
    fail_unless(cuda_required == TRUE);

    g_free(device);
    gst_object_unref(element);
}
GST_END_TEST

GST_START_TEST(test_ssvpub_exposes_helmet_event_properties) {
    GstElement *element = gst_element_factory_make("ssvpub", nullptr);
    fail_unless(element != nullptr);

    gboolean enabled = FALSE;
    gboolean publish_detection = FALSE;
    gchar *trigger_class = nullptr;
    gchar *evidence_dir = nullptr;
    g_object_get(element,
        "helmet-event-enabled", &enabled,
        "helmet-trigger-class", &trigger_class,
        "publish-detection-events", &publish_detection,
        "evidence-output-dir", &evidence_dir,
        nullptr);

    fail_unless(enabled == TRUE);
    fail_unless(publish_detection == TRUE);
    fail_unless(std::string(trigger_class) == "head");
    fail_unless(std::string(evidence_dir) == "artifacts/evidence");

    g_free(trigger_class);
    g_free(evidence_dir);
    gst_object_unref(element);
}
GST_END_TEST

GST_START_TEST(test_ssvpub_builds_helmet_violation_payload_for_head) {
    SsvFrameDetections frame{};
    frame.frame_id = 42;
    std::snprintf(frame.source_id, sizeof(frame.source_id), "camera-1");
    frame.detections.push_back(make_pub_det("helmet", 0, 0.95f, 6));
    frame.detections.push_back(make_pub_det("head", 1, 0.86f, 7));

    auto payload = ssv_pub_build_helmet_violation_payload(
        frame,
        1700000000000LL,
        "head",
        "artifacts/evidence/camera-1/1700000000000-frame-42.jpg",
        640,
        480,
        nullptr);

    fail_unless(payload.has_value());
    fail_unless(payload->find("\"type\":\"helmet_violation\"") != std::string::npos);
    fail_unless(payload->find("\"schema_version\":\"m4.helmet_violation.v1\"") != std::string::npos);
    fail_unless(payload->find("\"trigger_reason\":\"head_detected\"") != std::string::npos);
    fail_unless(payload->find("\"class\":\"head\"") != std::string::npos);
    fail_unless(payload->find("\"class\":\"helmet\"") == std::string::npos);
    fail_unless(payload->find("\"frame_path\":\"artifacts/evidence/camera-1/1700000000000-frame-42.jpg\"") != std::string::npos);
    fail_unless(payload->find("\"agent_state\":\"pending\"") != std::string::npos);
}
GST_END_TEST

GST_START_TEST(test_ssvpub_does_not_build_helmet_violation_without_head) {
    SsvFrameDetections frame{};
    frame.frame_id = 42;
    std::snprintf(frame.source_id, sizeof(frame.source_id), "camera-1");
    frame.detections.push_back(make_pub_det("helmet", 0, 0.95f, 6));

    auto payload = ssv_pub_build_helmet_violation_payload(
        frame, 1700000000000LL, "head", nullptr, 640, 480, nullptr);

    fail_unless(!payload.has_value());
}
GST_END_TEST

GST_START_TEST(test_ssvoverlay_runs_on_rgb_buffer) {
    GstElement *pipeline = gst_parse_launch(
        "videotestsrc num-buffers=1 ! video/x-raw,format=RGB,width=64,height=48 ! "
        "ssvoverlay ! fakesink", nullptr);
    fail_unless(pipeline != nullptr);

    GstStateChangeReturn ret = gst_element_set_state(pipeline, GST_STATE_PLAYING);
    fail_unless(ret != GST_STATE_CHANGE_FAILURE);

    GstBus *bus = gst_element_get_bus(pipeline);
    GstMessage *msg = gst_bus_timed_pop_filtered(
        bus, 5 * GST_SECOND,
        (GstMessageType)(GST_MESSAGE_ERROR | GST_MESSAGE_EOS));
    fail_unless(msg != nullptr);
    fail_unless(GST_MESSAGE_TYPE(msg) == GST_MESSAGE_EOS);

    gst_message_unref(msg);
    gst_object_unref(bus);
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);
}
GST_END_TEST

GST_START_TEST(test_ssvoverlay_runs_on_bgrx_buffer) {
    GstElement *pipeline = gst_parse_launch(
        "videotestsrc num-buffers=1 ! video/x-raw,format=BGRx,width=64,height=48 ! "
        "ssvoverlay ! fakesink", nullptr);
    fail_unless(pipeline != nullptr);

    GstStateChangeReturn ret = gst_element_set_state(pipeline, GST_STATE_PLAYING);
    fail_unless(ret != GST_STATE_CHANGE_FAILURE);

    GstBus *bus = gst_element_get_bus(pipeline);
    GstMessage *msg = gst_bus_timed_pop_filtered(
        bus, 5 * GST_SECOND,
        (GstMessageType)(GST_MESSAGE_ERROR | GST_MESSAGE_EOS));
    fail_unless(msg != nullptr);
    fail_unless(GST_MESSAGE_TYPE(msg) == GST_MESSAGE_EOS);

    gst_message_unref(msg);
    gst_object_unref(bus);
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);
}
GST_END_TEST

GST_START_TEST(test_ssvoverlay_draws_latest_detection) {
    auto &store = SsvDetectionStore::instance();
    (void)store.take();
    (void)store.take_for_tracking();

    SsvFrameDetections det{};
    det.frame_id = 1;
    std::snprintf(det.source_id, sizeof(det.source_id), "unit-test");
    SsvDetection d{};
    std::snprintf(d.class_name, sizeof(d.class_name), "person");
    d.confidence = 0.9f;
    d.x1 = 0.1f;
    d.y1 = 0.1f;
    d.x2 = 0.4f;
    d.y2 = 0.4f;
    d.class_id = 0;
    det.detections.push_back(d);
    store.set_tracked(std::move(det));

    GstElement *pipeline = gst_parse_launch(
        "videotestsrc num-buffers=1 pattern=black ! video/x-raw,format=BGRx,width=64,height=48 ! "
        "ssvoverlay ! appsink name=sink sync=false emit-signals=false max-buffers=1 drop=false", nullptr);
    fail_unless(pipeline != nullptr);

    GstStateChangeReturn ret = gst_element_set_state(pipeline, GST_STATE_PLAYING);
    fail_unless(ret != GST_STATE_CHANGE_FAILURE);

    GstElement *sink = gst_bin_get_by_name(GST_BIN(pipeline), "sink");
    fail_unless(sink != nullptr);
    GstSample *sample = gst_app_sink_pull_sample(GST_APP_SINK(sink));
    fail_unless(sample != nullptr);

    GstBuffer *buffer = gst_sample_get_buffer(sample);
    GstCaps *caps = gst_sample_get_caps(sample);
    GstVideoInfo info;
    gst_video_info_from_caps(&info, caps);
    GstVideoFrame frame;
    fail_unless(gst_video_frame_map(&frame, &info, buffer, GST_MAP_READ));
    const uint8_t *px = static_cast<const uint8_t *>(GST_VIDEO_FRAME_PLANE_DATA(&frame, 0));
    int stride = GST_VIDEO_FRAME_PLANE_STRIDE(&frame, 0);
    int x = 6;
    int y = 10;
    const uint8_t *p = px + y * stride + x * 4;
    fail_unless(p[0] == 0 && p[1] == 255 && p[2] == 0, "overlay did not draw green pixel");
    gst_video_frame_unmap(&frame);

    gst_sample_unref(sample);
    gst_object_unref(sink);
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);
}
GST_END_TEST

static Suite *ssv_gst_suite() {
    Suite *suite = suite_create("ssv-gst");
    TCase *tc = tcase_create("plugins");
    tcase_add_test(tc, test_ssv_plugin_factories_are_registered);
    tcase_add_test(tc, test_ssvinfer_exposes_label_map_property);
    tcase_add_test(tc, test_ssvinfer_exposes_device_properties);
    tcase_add_test(tc, test_ssvpub_exposes_helmet_event_properties);
    tcase_add_test(tc, test_ssvpub_builds_helmet_violation_payload_for_head);
    tcase_add_test(tc, test_ssvpub_does_not_build_helmet_violation_without_head);
    tcase_add_test(tc, test_ssvoverlay_runs_on_rgb_buffer);
    tcase_add_test(tc, test_ssvoverlay_runs_on_bgrx_buffer);
    tcase_add_test(tc, test_ssvoverlay_draws_latest_detection);
    suite_add_tcase(suite, tc);
    return suite;
}

int main(int argc, char **argv) {
    gst_check_init(&argc, &argv);
    run_ssv_meta_tests();

    Suite *suite = ssv_gst_suite();
    SRunner *runner = srunner_create(suite);
    srunner_run_all(runner, CK_NORMAL);
    int failed = srunner_ntests_failed(runner);
    srunner_free(runner);
    return failed == 0 ? 0 : 1;
}
