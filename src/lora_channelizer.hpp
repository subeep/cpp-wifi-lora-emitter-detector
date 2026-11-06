// FFT (overlap-save, fast-convolution) channelizer for wideband LoRa listening.
//
// Extracts several narrow channels from one wide capture in a single pass:
// each block of the input is transformed once, and every channel takes its
// own M bins around its centre, shapes them with a flat-top mask and inverse
// transforms at the output rate. Nothing full-length is copied per channel,
// and blocks run on several threads, so a 6 s, 20 Msps capture (120 M samples)
// costs one parallel FFT pass instead of a full-rate mix + FIR per channel.
//
// Output sample k corresponds to input sample k*D (D = in_rate/out_rate), the
// same alignment as channelize.hpp's derive_channel(), so capture timing and
// sample indices scale exactly as before. Pass band +/-0.48*out_rate, falling
// to zero at +/-0.5*out_rate (the same edges derive_channel() uses).
#pragma once
#include <complex>
#include <vector>

namespace rfmon::lora_channelizer {

// offsets_hz: each channel centre minus the capture centre. in_rate/out_rate
// must be an integer >= 2; every |offset| + out_rate/2 must be below
// in_rate/2. Throws std::invalid_argument otherwise. threads 0 = hardware.
std::vector<std::vector<std::complex<float>>> extract(const std::vector<std::complex<float>>& wide, double in_rate_hz,
                                                      const std::vector<double>& offsets_hz, double out_rate_hz,
                                                      unsigned threads = 0);

}  // namespace rfmon::lora_channelizer
