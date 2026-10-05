#pragma once

#include <complex>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace rfmon::cyclo {

enum class IqFormat { cf32_le, ci16_le };
IqFormat parse_format(const std::string& name);
const char* format_name(IqFormat format);
std::uint64_t bytes_per_sample(IqFormat format);

struct IqWindow {
    std::filesystem::path path;
    std::uint64_t file_bytes = 0;
    std::uint64_t file_samples = 0;
    std::uint64_t offset_samples = 0;
    std::vector<std::complex<float>> samples;
};

// Headerless interleaved I,Q only. Reads at most 8 MiB of IQ; originals are
// never modified. Rate, center and continuity cannot be inferred from bytes.
constexpr std::uint64_t max_window_samples = 1048576;
IqWindow read_iq_window(const std::filesystem::path& path, IqFormat format,
                        std::uint64_t offset_samples, std::uint64_t count);

} // namespace rfmon::cyclo
