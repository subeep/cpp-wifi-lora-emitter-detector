#include "iq_file.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <stdexcept>

namespace rfmon::cyclo {
namespace {
float decode_float(const unsigned char* p) {
    static_assert(sizeof(float) == 4 && std::numeric_limits<float>::is_iec559,
                  "IEEE float32 required");
    std::uint32_t bits = std::uint32_t(p[0]) | (std::uint32_t(p[1]) << 8) |
                         (std::uint32_t(p[2]) << 16) | (std::uint32_t(p[3]) << 24);
    float value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}
float decode_int16(const unsigned char* p) {
    int value = int(p[0]) | (int(p[1]) << 8);
    if (value >= 32768) value -= 65536;
    return float(value) / 32768.0f;
}
} // namespace

IqFormat parse_format(const std::string& name) {
    if (name == "cf32_le") return IqFormat::cf32_le;
    if (name == "ci16_le") return IqFormat::ci16_le;
    throw std::invalid_argument("format must be cf32_le or ci16_le");
}
const char* format_name(IqFormat format) {
    return format == IqFormat::cf32_le ? "cf32_le" : "ci16_le";
}
std::uint64_t bytes_per_sample(IqFormat format) {
    return format == IqFormat::cf32_le ? 8 : 4;
}

IqWindow read_iq_window(const std::filesystem::path& path, IqFormat format,
                        std::uint64_t offset, std::uint64_t count) {
    if (!count || count > max_window_samples)
        throw std::invalid_argument("sample count must be in 1..1048576");
    if (!std::filesystem::is_regular_file(path))
        throw std::invalid_argument("IQ input must be a regular file");
    IqWindow result;
    result.path = std::filesystem::canonical(path);
    result.file_bytes = std::filesystem::file_size(result.path);
    const auto stride = bytes_per_sample(format);
    if (!result.file_bytes || result.file_bytes % stride)
        throw std::invalid_argument("empty or misaligned headerless IQ file");
    result.file_samples = result.file_bytes / stride;
    if (offset >= result.file_samples)
        throw std::invalid_argument("offset is outside the IQ file");
    result.offset_samples = offset;
    count = std::min(count, result.file_samples - offset);
    const auto byte_offset = offset * stride; // bounded by file_bytes
    if (byte_offset > std::uint64_t(std::numeric_limits<std::streamoff>::max()))
        throw std::invalid_argument("file offset exceeds stream range");
    std::ifstream stream(result.path, std::ios::binary);
    stream.seekg(std::streamoff(byte_offset));
    if (!stream) throw std::runtime_error("cannot seek IQ input");
    std::vector<unsigned char> bytes(std::size_t(count * stride));
    stream.read(reinterpret_cast<char*>(bytes.data()), std::streamsize(bytes.size()));
    if (!stream || std::uint64_t(stream.gcount()) != bytes.size())
        throw std::runtime_error("short IQ read; file may have changed");
    result.samples.reserve(std::size_t(count));
    for (std::size_t i = 0; i < count; ++i) {
        const auto* p = bytes.data() + i * stride;
        float re, im;
        if (format == IqFormat::cf32_le) {
            re = decode_float(p); im = decode_float(p + 4);
        } else {
            re = decode_int16(p); im = decode_int16(p + 2);
        }
        if (!std::isfinite(re) || !std::isfinite(im))
            throw std::invalid_argument("non-finite IQ at sample " +
                                        std::to_string(offset + i));
        result.samples.emplace_back(re, im);
    }
    return result;
}
} // namespace rfmon::cyclo
