#include "ssv_frame_evidence.hpp"

#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

#include <jpeglib.h>

namespace {

std::array<std::uint8_t, 48> make_bgr_fixture_with_padding()
{
    std::array<std::uint8_t, 48> pixels = {};
    for (std::size_t row = 0; row < 3; ++row) {
        for (std::size_t column = 0; column < 4; ++column) {
            const auto offset = row * 16 + column * 3;
            pixels[offset] = static_cast<std::uint8_t>(10 + row + column);
            pixels[offset + 1] = static_cast<std::uint8_t>(30 + row + column);
            pixels[offset + 2] = static_cast<std::uint8_t>(50 + row + column);
        }
        for (std::size_t padding = 12; padding < 16; ++padding) {
            pixels[row * 16 + padding] = 0xff;
        }
    }
    return pixels;
}

std::pair<int, int> decode_jpeg_dimensions(const std::filesystem::path &path)
{
    std::ifstream input(path, std::ios::binary);
    const auto bytes = std::vector<unsigned char>(
        std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    jpeg_decompress_struct decoder = {};
    jpeg_error_mgr error_manager = {};
    decoder.err = jpeg_std_error(&error_manager);
    jpeg_create_decompress(&decoder);
    jpeg_mem_src(&decoder, bytes.data(), bytes.size());
    assert(jpeg_read_header(&decoder, TRUE) == JPEG_HEADER_OK);
    assert(jpeg_start_decompress(&decoder));
    const auto dimensions = std::pair(
        static_cast<int>(decoder.output_width), static_cast<int>(decoder.output_height));
    jpeg_destroy_decompress(&decoder);
    return dimensions;
}

} // namespace

int main()
{
    const auto pixels = make_bgr_fixture_with_padding();
    std::string error;
    const auto evidence = ssv_encode_bgr_jpeg(pixels.data(), 4, 3, 16, 90, &error);
    assert(evidence.has_value());
    assert(evidence->width == 4);
    assert(evidence->height == 3);
    assert(evidence->sha256.size() == 64);

    const auto temp_dir = std::filesystem::temp_directory_path() / "ssv-frame-evidence-test";
    std::filesystem::create_directories(temp_dir);
    const auto output = temp_dir / "evidence.jpg";
    assert(ssv_write_atomic_bytes(output, evidence->jpeg_bytes, &error));
    assert(std::filesystem::exists(output));
    for (const auto &entry : std::filesystem::directory_iterator(temp_dir)) {
        assert(entry.path().filename().string().rfind(".evidence.jpg.tmp-", 0) != 0);
    }
    assert(decode_jpeg_dimensions(output) == std::pair(4, 3));
    assert(ssv_sha256_hex(evidence->jpeg_bytes) == evidence->sha256);

    assert(!ssv_encode_bgr_jpeg(nullptr, 4, 3, 16, 90, &error));
    assert(!ssv_encode_bgr_jpeg(pixels.data(), 0, 3, 16, 90, &error));
    assert(!ssv_encode_bgr_jpeg(pixels.data(), 4, 3, 11, 90, &error));
    assert(!ssv_encode_bgr_jpeg(pixels.data(), 4, 3, 16, 80, &error));
    return 0;
}
