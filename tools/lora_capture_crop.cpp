// Crops a saved LoRa IQ capture to a sample-index range (plus margin),
// producing a new, independently-saved capture in the SAME format -
// for turning a long coordinated real-transmission window into small,
// individually-labeled fixtures (EXECUTE_NEXT.md Section 6's tracked
// regression corpus). Reuses load_lora_capture()/save_lora_capture()
// directly rather than hand-writing the manifest/checksum, so the
// output is guaranteed format-valid by construction, not by a second,
// possibly-diverging implementation of the same write path.
//
// This does not attempt to locate the burst itself - the caller
// supplies the sample range (e.g. from a prior energy scan). No signal
// processing happens here at all; original samples are copied verbatim
// within the requested range, nothing is filtered, scaled, or
// resampled.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>

#include "lora_capture.hpp"

using namespace rfmon;

int main(int argc, char** argv) {
    if (argc < 5) {
        std::fprintf(stderr,
            "Usage: %s SOURCE_CAPTURE_DIR START_S END_S OUT_PARENT_DIR [MARGIN_S]\n"
            "  Crops [START_S - MARGIN_S, END_S + MARGIN_S] (clamped to the source's own\n"
            "  bounds) from SOURCE_CAPTURE_DIR's IQ and saves it as a new capture under\n"
            "  OUT_PARENT_DIR, in the same manifest+cf32 format. MARGIN_S defaults to 0.3s.\n",
            argv[0]);
        return 2;
    }
    std::string src_dir = argv[1];
    double start_s = std::stod(argv[2]);
    double end_s = std::stod(argv[3]);
    std::string out_parent = argv[4];
    double margin_s = (argc > 5) ? std::stod(argv[5]) : 0.3;

    try {
        LoraCapture src = load_lora_capture(src_dir);
        double rate = src.sample_rate_hz;
        long total = long(src.iq.size());

        long start_idx = std::max(0L, long((start_s - margin_s) * rate));
        long end_idx = std::min(total, long((end_s + margin_s) * rate) + 1);
        if (start_idx >= end_idx) {
            std::fprintf(stderr, "FAIL: empty/invalid crop range after clamping (%ld..%ld of %ld)\n",
                         start_idx, end_idx, total);
            return 1;
        }

        LoraCapture out;
        out.iq.assign(src.iq.begin() + start_idx, src.iq.begin() + end_idx);
        out.sample_rate_hz = src.sample_rate_hz;
        out.requested_sample_rate_hz = src.requested_sample_rate_hz;
        out.requested_center_hz = src.requested_center_hz;
        out.requested_duration_s = double(out.iq.size()) / rate;
        out.host_start_unix_s = src.host_start_unix_s + double(start_idx) / rate;
        out.requested_gain_db = src.requested_gain_db;
        out.overflow = src.overflow;
        out.device_args = src.device_args;
        out.antenna = src.antenna;
        out.source = "live-cropped:" + src_dir;

        std::string dir = save_lora_capture(out_parent, out);
        std::printf("%s\n", dir.c_str());
        std::fprintf(stderr, "Cropped %ld..%ld of %ld samples (%.3fs..%.3fs of %.3fs) -> %zu samples (%.3fs)\n",
                     start_idx, end_idx, total, double(start_idx) / rate, double(end_idx) / rate,
                     double(total) / rate, out.iq.size(), out.requested_duration_s);
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FAIL: %s\n", e.what());
        return 1;
    }
}
