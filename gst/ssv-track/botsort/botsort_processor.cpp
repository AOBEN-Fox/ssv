#include "botsort_processor.hpp"
#include "botsort_coordinates.hpp"

#include <utility>
namespace botsort {
namespace {
Detection ssv_to_botsort_detection(const SsvDetection &src, int input_index) {
    Detection det;
    det.x1 = src.x1; det.y1 = src.y1; det.x2 = src.x2; det.y2 = src.y2;
    det.score = src.confidence; det.class_id = src.class_id;
    det.class_name = src.class_name; det.input_index = input_index;
    return det;
}
FrameDetections ssv_to_botsort_detections(const std::vector<SsvDetection> &src, int width, int height) {
    FrameDetections out;
    out.reserve(src.size());
    for (std::size_t i = 0; i < src.size(); ++i)
        out.push_back(to_pixel_detection(ssv_to_botsort_detection(src[i], static_cast<int>(i)), width, height));
    return out;
}
std::vector<SsvTrackedObject> make_tracked_objects(
    std::vector<SsvDetection> detections,
    const FrameDetections &results) {
    std::vector<SsvTrackedObject> objects;
    objects.reserve(detections.size());
    for (auto &detection : detections) {
        SsvTrackedObject object;
        object.detection = std::move(detection);
        objects.push_back(std::move(object));
    }
    for (const auto &det : results) {
        if (det.input_index < 0) continue;
        const std::size_t index = static_cast<std::size_t>(det.input_index);
        if (index >= objects.size()) continue;
        auto &out = objects[index];
        out.track_id = det.track_id;
        out.track_state = static_cast<SsvTrackState>(det.track_state);
        out.occluded = det.occluded;
    }
    return objects;
}
}
BoTSortProcessor::BoTSortProcessor(TrackerConfig config) : tracker_(config) {}
std::vector<SsvTrackedObject> BoTSortProcessor::process(
    std::vector<SsvDetection> detections,
    int frame_width,
    int frame_height,
    const std::uint8_t *frame_data,
    std::size_t frame_stride) {
    auto input = ssv_to_botsort_detections(detections, frame_width, frame_height);
    FrameView frame_view;
    frame_view.data = frame_data;
    frame_view.width = frame_width;
    frame_view.height = frame_height;
    frame_view.stride = frame_stride;
    UpdateResult result = frame_data != nullptr ? tracker_.update(input, frame_view) : tracker_.update(input);
    for (auto &tracked : result.detections)
        tracked = to_normalized_detection(tracked, frame_width, frame_height);
    return make_tracked_objects(std::move(detections), result.detections);
}
}  // namespace botsort
