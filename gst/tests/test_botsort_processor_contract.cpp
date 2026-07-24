#include "botsort/botsort_processor.hpp"

#include <cassert>
#include <cstdio>
#include <utility>
#include <vector>

int main()
{
    botsort::TrackerConfig config;
    config.gmc_method = botsort::GmcMethod::kNone;
    botsort::BoTSortProcessor processor(config);

    SsvDetection detection;
    std::snprintf(detection.class_name, sizeof(detection.class_name), "person");
    detection.confidence = 0.9F;
    detection.x1 = 0.1F;
    detection.y1 = 0.2F;
    detection.x2 = 0.3F;
    detection.y2 = 0.5F;
    detection.class_id = 0;
    std::vector<SsvDetection> detections;
    detections.push_back(detection);

    auto tracked = processor.process(std::move(detections), 640, 480);
    assert(tracked.size() == 1);
    assert(tracked.front().detection.class_id == 0);
    assert(tracked.front().detection.x1 == detection.x1);
    assert(tracked.front().track_id >= 0);
    assert(tracked.front().track_state == SSV_TRACK_NEW);
    assert(!tracked.front().occluded);
    return 0;
}
