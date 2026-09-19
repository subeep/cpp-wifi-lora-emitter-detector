// ImGui/ImPlot rendering for the Wi-Fi test bench - a standalone window,
// no relation to wifi_gui.hpp/.cpp (the main app's tables), though it
// mirrors that file's visual conventions (row coloring, "--"/"n/a"
// distinction, click-a-row-for-details) so it feels like the same
// project.
#pragma once

#include "bench_capture.hpp"
#include "bench_tx.hpp"

namespace rfmon::bench {

// Draws the entire test-bench window for one frame: connect controls,
// frequency lock, the top-half IQ/FFT plots, the bottom-half packet
// table, and (once disconnected) the playback transport - plus, on the
// Live Capture tab, the TX loopback controls (see bench_tx.hpp). Owns
// all of its own UI state via static locals, matching main.cpp's
// convention - call once per frame from the render loop.
void draw_bench_window(BenchCapture& capture, BenchTx& tx);

}  // namespace rfmon::bench
