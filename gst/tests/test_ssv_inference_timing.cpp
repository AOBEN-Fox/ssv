#include "ssv_inference_engine.hpp"

#include <cassert>

int main()
{
    ssv::infer::SsvVideoFrame input;
    input.frame_id = 0;
    input.source_id = "camera-01";
    input.timing = {5 * GST_SECOND, GST_SECOND / 5, 7};

    ssv::infer::InferenceEngine engine;
    auto output = engine.run(input);

    assert(output.frame_id == 0);
    assert(output.source_id == "camera-01");
    assert(output.timing == input.timing);
    assert(output.detections.empty());
    return 0;
}
