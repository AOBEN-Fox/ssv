#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

struct SsvFrameEvidence {
    std::vector<std::uint8_t> jpeg_bytes;
    std::string sha256;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

std::optional<SsvFrameEvidence> ssv_encode_bgr_jpeg(
    const std::uint8_t *data,
    std::uint32_t width,
    std::uint32_t height,
    std::size_t stride,
    int quality,
    std::string *error);

std::string ssv_sha256_hex(std::span<const std::uint8_t> bytes);

bool ssv_write_atomic_bytes(
    const std::filesystem::path &final_path,
    std::span<const std::uint8_t> bytes,
    std::string *error);
