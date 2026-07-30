#include "ssv_frame_evidence.hpp"

#include <glib.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <fstream>
#include <memory>
#include <string>
#include <system_error>
#include <utility>

#include <jpeglib.h>

#include <unistd.h>

namespace {

void set_error(std::string *error, std::string value)
{
    if (error != nullptr) {
        *error = std::move(value);
    }
}

} // namespace

std::string ssv_sha256_hex(std::span<const std::uint8_t> bytes)
{
    const std::unique_ptr<GChecksum, decltype(&g_checksum_free)> checksum(
        g_checksum_new(G_CHECKSUM_SHA256), g_checksum_free);
    g_checksum_update(checksum.get(), bytes.data(), bytes.size());
    return g_checksum_get_string(checksum.get());
}

std::optional<SsvFrameEvidence> ssv_encode_bgr_jpeg(
    const std::uint8_t *data,
    std::uint32_t width,
    std::uint32_t height,
    std::size_t stride,
    int quality,
    std::string *error)
{
    if (data == nullptr || width == 0 || height == 0 || stride < width * 3U) {
        set_error(error, "BGR 图像参数无效");
        return std::nullopt;
    }
    if (quality != 90) {
        set_error(error, "JPEG quality 必须为 90");
        return std::nullopt;
    }

    jpeg_compress_struct encoder = {};
    jpeg_error_mgr error_manager = {};
    encoder.err = jpeg_std_error(&error_manager);
    jpeg_create_compress(&encoder);

    unsigned char *encoded = nullptr;
    unsigned long encoded_size = 0;
    jpeg_mem_dest(&encoder, &encoded, &encoded_size);
    encoder.image_width = width;
    encoder.image_height = height;
    encoder.input_components = 3;
    encoder.in_color_space = JCS_RGB;
    jpeg_set_defaults(&encoder);
    jpeg_set_quality(&encoder, quality, TRUE);
    jpeg_start_compress(&encoder, TRUE);

    std::vector<std::uint8_t> rgb_row(static_cast<std::size_t>(width) * 3U);
    while (encoder.next_scanline < encoder.image_height) {
        const auto *bgr = data + static_cast<std::size_t>(encoder.next_scanline) * stride;
        for (std::size_t column = 0; column < width; ++column) {
            const auto bgr_offset = column * 3U;
            rgb_row[bgr_offset] = bgr[bgr_offset + 2U];
            rgb_row[bgr_offset + 1U] = bgr[bgr_offset + 1U];
            rgb_row[bgr_offset + 2U] = bgr[bgr_offset];
        }
        JSAMPROW row = rgb_row.data();
        jpeg_write_scanlines(&encoder, &row, 1);
    }
    jpeg_finish_compress(&encoder);

    SsvFrameEvidence evidence;
    evidence.jpeg_bytes.assign(encoded, encoded + encoded_size);
    std::free(encoded);
    jpeg_destroy_compress(&encoder);
    evidence.sha256 = ssv_sha256_hex(evidence.jpeg_bytes);
    evidence.width = width;
    evidence.height = height;
    return evidence;
}

bool ssv_write_atomic_bytes(
    const std::filesystem::path &final_path,
    std::span<const std::uint8_t> bytes,
    std::string *error)
{
    const auto parent = final_path.parent_path();
    if (parent.empty()) {
        set_error(error, "证据路径必须包含父目录");
        return false;
    }

    std::error_code filesystem_error;
    std::filesystem::create_directories(parent, filesystem_error);
    if (filesystem_error) {
        set_error(error, "无法创建证据目录: " + filesystem_error.message());
        return false;
    }

    const auto temporary = parent /
        ("." + final_path.filename().string() + ".tmp-" + std::to_string(getpid()));
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) {
            set_error(error, "无法创建临时证据文件");
            return false;
        }
        output.write(
            reinterpret_cast<const char *>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
        output.close();
        if (!output) {
            std::filesystem::remove(temporary, filesystem_error);
            set_error(error, "无法写入临时证据文件");
            return false;
        }
    }

    std::filesystem::rename(temporary, final_path, filesystem_error);
    if (filesystem_error) {
        std::filesystem::remove(temporary, filesystem_error);
        set_error(error, "无法原子替换证据文件");
        return false;
    }
    return true;
}
