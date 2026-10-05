#pragma once
#include "ofdm_structure.hpp"
#include "spectral_correlation.hpp"
#include "sample_tiles.hpp"
#include "roi_measurements.hpp"
#include "chirp_structure.hpp"
#include "link_evidence.hpp"
#include <cstdint>
#include <stdexcept>

namespace rfmon::cyclo {
using WireBytes = std::vector<unsigned char>;
inline constexpr std::uint32_t wire_magic = 0x314c4352, wire_version = 4;
inline constexpr std::size_t request_byte_limit = 32 + tile_limit * 12 + capture_sample_budget * 8;
inline constexpr std::size_t reply_byte_limit = 65536;
enum class MessageType : std::uint32_t { hello = 1, request = 2, result = 3, error = 4 };
struct WireHeader { MessageType type; std::uint32_t bytes; };
WireBytes encode_header(MessageType type, std::size_t bytes);
WireHeader decode_header(const WireBytes& header, std::size_t byte_limit);

struct TileMeasurement {
    SampleTile source;
    SpectralFeatures spectral;
    std::vector<OfdmMeasurement> ofdm;
    RoiMeasurements roi;
    std::vector<ChirpMeasurement> chirps;
    LinkEvidence evidence; // computed by supervisor after strict wire validation; not sent by child
};
struct WorkerTile { SampleTile source; std::vector<std::complex<float>> iq; };
struct WorkerRequest { std::uint64_t job_id; double rate_hz; std::vector<WorkerTile> tiles; };
WireBytes encode_request(std::uint64_t job_id, double rate_hz, const TilePlan& plan,
                         const std::vector<std::complex<float>>& packed_iq);
WorkerRequest decode_request(const WireBytes& bytes);
WireBytes encode_result(std::uint64_t job_id, const std::vector<TileMeasurement>& tiles);
std::vector<TileMeasurement> decode_result(const WireBytes& bytes, std::uint64_t job_id,
                                          double rate_hz, const TilePlan& plan);
WireBytes encode_error(std::uint64_t job_id);
void validate_error(const WireBytes& bytes, std::uint64_t job_id);
} // namespace rfmon::cyclo
