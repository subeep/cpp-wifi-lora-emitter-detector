#include "bench_gui.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "imgui.h"
#include "implot.h"

#include "config.hpp"
#include "spectrum.hpp"

namespace rfmon::bench {

namespace {

// Every numeric column the packet table can plot over time - None marks
// a non-numeric column (Time/Modulation/SSID/...) that TableGetSortSpecs()
// can still report a click on, which the header-click handler below
// just ignores.
enum class BenchParam {
    None, Power, Bandwidth, Duration, Confidence, Cfo, Irr, IqEps, IqPhi,
    Dc, DcAng, Snr, Evm, SyncCorr, NSamp,
};

std::optional<double> param_value(const BenchPacketRow& r, BenchParam p) {
    switch (p) {
        case BenchParam::Power: return r.power_db;
        case BenchParam::Bandwidth: return r.bandwidth_khz;
        case BenchParam::Duration: return r.duration_us;
        case BenchParam::Confidence: return r.confidence;
        case BenchParam::Cfo: return r.fp_cfo_ppm;
        case BenchParam::Irr: return r.fp_irr_db;
        case BenchParam::IqEps: return r.fp_iq_eps;
        case BenchParam::IqPhi: return r.fp_iq_phi_deg;
        case BenchParam::Dc: return r.fp_dc_dbc;
        case BenchParam::DcAng: return r.fp_dc_ang_deg;
        case BenchParam::Snr: return r.fp_snr_db;
        case BenchParam::Evm: return r.fp_evm_pct;
        case BenchParam::SyncCorr: return r.fp_sync_corr;
        case BenchParam::NSamp: return r.fp_n_samp.has_value() ? std::optional<double>(*r.fp_n_samp) : std::nullopt;
        default: return std::nullopt;
    }
}

const char* param_label(BenchParam p) {
    switch (p) {
        case BenchParam::Power: return "Power (dB)";
        case BenchParam::Bandwidth: return "Bandwidth (kHz)";
        case BenchParam::Duration: return "Duration (us)";
        case BenchParam::Confidence: return "Confidence";
        case BenchParam::Cfo: return "CFO (ppm)";
        case BenchParam::Irr: return "IRR (dB)";
        case BenchParam::IqEps: return "IQ eps";
        case BenchParam::IqPhi: return "IQ phi (deg)";
        case BenchParam::Dc: return "DC (dBc)";
        case BenchParam::DcAng: return "DC angle (deg)";
        case BenchParam::Snr: return "SNR (dB)";
        case BenchParam::Evm: return "EVM (%)";
        case BenchParam::SyncCorr: return "Sync corr";
        case BenchParam::NSamp: return "N samples";
        default: return "";
    }
}

struct ColumnSpec {
    const char* label;
    float width;
    BenchParam param;  // None for non-numeric / non-plottable columns
};

// One entry per table column, in display order - also what the
// sort-click handler maps a clicked column index back to a parameter
// through (see draw_packet_table()).
const std::vector<ColumnSpec>& bench_columns() {
    static const std::vector<ColumnSpec> cols = {
        {"Time", 80.0f, BenchParam::None},
        {"Freq (MHz)", 95.0f, BenchParam::None},
        {"Modulation", 95.0f, BenchParam::None},
        {"Power (dB)", 90.0f, BenchParam::Power},
        {"BW (MHz)", 85.0f, BenchParam::Bandwidth},
        {"Duration (us)", 105.0f, BenchParam::Duration},
        {"Confidence", 90.0f, BenchParam::Confidence},
        {"SSID", 150.0f, BenchParam::None},
        {"BSSID", 140.0f, BenchParam::None},
        {"CFO (ppm)", 90.0f, BenchParam::Cfo},
        {"IRR (dB)", 80.0f, BenchParam::Irr},
        {"IQ eps", 80.0f, BenchParam::IqEps},
        {"IQ phi (deg)", 100.0f, BenchParam::IqPhi},
        {"DC (dBc)", 80.0f, BenchParam::Dc},
        {"DC angle (deg)", 110.0f, BenchParam::DcAng},
        {"SNR (dB)", 85.0f, BenchParam::Snr},
        {"EVM (%)", 80.0f, BenchParam::Evm},
        {"Sync corr", 90.0f, BenchParam::SyncCorr},
        {"N samp", 75.0f, BenchParam::NSamp},
        {"Gate reason", 200.0f, BenchParam::None},
    };
    return cols;
}

// Strided view for plotting - math upstream always runs on the full-
// rate buffer; this only limits how many points ImPlot has to lay out
// on screen for a burst that can be tens of thousands of samples long.
constexpr int kMaxPlotPoints = 4000;

void draw_iq_fft_plots(const std::vector<std::complex<float>>& iq, double sample_rate_hz,
                        double capture_center_hz, double segment_center_hz, float height,
                        const char* id_suffix) {
    if (iq.empty() || sample_rate_hz <= 0.0) {
        ImGui::TextDisabled("No capture yet - connect and lock a frequency to see live IQ/FFT.");
        return;
    }

    static int iq_view_mode = 0;  // 0 = time domain, 1 = constellation
    ImGui::RadioButton("Time domain", &iq_view_mode, 0);
    ImGui::SameLine();
    ImGui::RadioButton("Constellation", &iq_view_mode, 1);

    float avail_w = ImGui::GetContentRegionAvail().x;
    float plot_w = avail_w * 0.5f - 4.0f;
    float plot_h = height - ImGui::GetFrameHeightWithSpacing();

    int stride = std::max<int>(1, int(iq.size()) / kMaxPlotPoints);
    int n_plot = int(iq.size()) / stride;
    std::vector<double> xs(n_plot), is(n_plot), qs(n_plot);
    for (int i = 0; i < n_plot; ++i) {
        const auto& s = iq[size_t(i) * size_t(stride)];
        xs[i] = double(i * stride) / sample_rate_hz * 1e6;  // microseconds
        is[i] = double(s.real());
        qs[i] = double(s.imag());
    }

    std::string iq_title = std::string("IQ##") + id_suffix;
    if (ImPlot::BeginPlot(iq_title.c_str(), ImVec2(plot_w, plot_h))) {
        if (iq_view_mode == 0) {
            ImPlot::SetupAxes("Time (us)", "Amplitude");
            ImPlot::PlotLine("I", xs.data(), is.data(), n_plot);
            ImPlot::PlotLine("Q", xs.data(), qs.data(), n_plot);
        } else {
            ImPlot::SetupAxes("I", "Q");
            ImPlot::SetupAxisLimitsConstraints(ImAxis_X1, -1e9, 1e9);
            ImPlot::PlotScatter("samples", is.data(), qs.data(), n_plot);
        }
        ImPlot::EndPlot();
    }

    ImGui::SameLine();

    rfmon::Spectrum spec = rfmon::spectrogram_max_db(iq, sample_rate_hz);
    std::vector<double> freq_mhz(spec.freqs_offset_hz.size());
    for (size_t i = 0; i < freq_mhz.size(); ++i) {
        freq_mhz[i] = (capture_center_hz + spec.freqs_offset_hz[i]) / 1e6;
    }
    std::string fft_title = std::string("FFT##") + id_suffix;
    if (ImPlot::BeginPlot(fft_title.c_str(), ImVec2(plot_w, plot_h))) {
        ImPlot::SetupAxes("Frequency (MHz)", "Power (dB)");
        if (!freq_mhz.empty()) {
            ImPlot::PlotLine("PSD", freq_mhz.data(), spec.psd_db.data(), int(freq_mhz.size()));
            double target_mhz = segment_center_hz / 1e6;
            double xs2[2] = {target_mhz, target_mhz};
            double ys2[2] = {*std::min_element(spec.psd_db.begin(), spec.psd_db.end()),
                              *std::max_element(spec.psd_db.begin(), spec.psd_db.end())};
            ImPlot::PlotLine("Target freq", xs2, ys2, 2);
        }
        ImPlot::EndPlot();
    }
}

// Returns the packet clicked on (via the Time cell), or nullopt if none
// this frame. `*param_out` is set to whichever parameter's header was
// clicked, via ImGui's own sort-request mechanism - see the comment at
// TableGetSortSpecs() below for why this deliberately never sorts.
std::optional<size_t> draw_packet_table(const std::vector<BenchPacketPtr>& packets, float height,
                                          BenchParam* param_out) {
    std::optional<size_t> clicked_row;
    if (packets.empty()) {
        ImGui::TextDisabled("No packets classified yet.");
        return clicked_row;
    }
    const auto& cols = bench_columns();
    static ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable |
                                    ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY |
                                    ImGuiTableFlags_ScrollX | ImGuiTableFlags_Sortable |
                                    ImGuiTableFlags_SortTristate;
    if (!ImGui::BeginTable("bench_packets", int(cols.size()), flags, ImVec2(0, height))) {
        return clicked_row;
    }
    ImGui::TableSetupScrollFreeze(0, 1);
    for (size_t i = 0; i < cols.size(); ++i) {
        ImGuiTableColumnFlags cf = ImGuiTableColumnFlags_WidthFixed;
        if (cols[i].param == BenchParam::None) cf |= ImGuiTableColumnFlags_NoSort;
        ImGui::TableSetupColumn(cols[i].label, cf, cols[i].width);
    }
    ImGui::TableHeadersRow();

    // A click on a sortable header arrives here as a normal sort
    // request. Rather than honoring it (which would reorder the log
    // away from chronological order), read which column it named,
    // surface that as "graph this parameter", and mark the request
    // consumed - the table never actually re-sorts.
    if (ImGuiTableSortSpecs* specs = ImGui::TableGetSortSpecs()) {
        if (specs->SpecsDirty && specs->SpecsCount > 0) {
            int col_idx = specs->Specs[0].ColumnIndex;
            if (col_idx >= 0 && col_idx < int(cols.size()) && cols[size_t(col_idx)].param != BenchParam::None) {
                *param_out = cols[size_t(col_idx)].param;
            }
            specs->SpecsDirty = false;
        }
    }

    const ImVec4 dsss_color(0.36f, 0.68f, 0.93f, 1.0f);
    const ImVec4 ofdm_color(0.25f, 0.73f, 0.31f, 1.0f);
    const ImVec4 dim(0.55f, 0.55f, 0.58f, 1.0f);
    const ImVec4 bad(0.85f, 0.45f, 0.40f, 1.0f);

    auto opt_cell = [&](const std::optional<double>& v, const char* fmt) {
        ImGui::TableNextColumn();
        if (v.has_value()) ImGui::Text(fmt, *v);
        else ImGui::TextColored(dim, "--");
    };

    for (size_t i = packets.size(); i-- > 0;) {
        const auto& p = *packets[i];
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        char label[32];
        std::snprintf(label, sizeof(label), "%s##%llu", p.time.c_str(),
                      static_cast<unsigned long long>(p.seq));
        if (ImGui::Selectable(label, false, ImGuiSelectableFlags_SpanAllColumns)) clicked_row = i;
        ImGui::TableNextColumn();
        ImGui::Text("%.4f", p.freq_mhz);
        ImGui::TableNextColumn();
        ImGui::TextColored(p.modulation == "DSSS" ? dsss_color : ofdm_color, "%s", p.modulation.c_str());
        ImGui::TableNextColumn();
        ImGui::Text("%.1f", p.power_db);
        ImGui::TableNextColumn();
        ImGui::Text("%.2f", p.bandwidth_khz / 1e3);
        ImGui::TableNextColumn();
        ImGui::Text("%.1f", p.duration_us);
        ImGui::TableNextColumn();
        ImGui::Text("%.3f", p.confidence);
        ImGui::TableNextColumn();
        if (p.identity) ImGui::TextUnformatted(p.identity->ssid.empty() ? "Hidden/absent" : wifi::display_text(p.identity->ssid).c_str());
        else ImGui::TextColored(dim, "--");
        ImGui::TableNextColumn();
        if (p.identity) ImGui::TextUnformatted(p.identity->bssid.c_str());
        else ImGui::TextColored(dim, "--");
        opt_cell(p.fp_cfo_ppm, "%.2f");
        if (p.modulation == "DSSS") {
            ImGui::TableNextColumn(); ImGui::TextColored(dim, "n/a");
            ImGui::TableNextColumn(); ImGui::TextColored(dim, "n/a");
            ImGui::TableNextColumn(); ImGui::TextColored(dim, "n/a");
        } else {
            opt_cell(p.fp_irr_db, "%.2f");
            opt_cell(p.fp_iq_eps, "%.4f");
            opt_cell(p.fp_iq_phi_deg, "%.2f");
        }
        opt_cell(p.fp_dc_dbc, "%.2f");
        opt_cell(p.fp_dc_ang_deg, "%.2f");
        opt_cell(p.fp_snr_db, "%.1f");
        opt_cell(p.fp_evm_pct, "%.1f");
        opt_cell(p.fp_sync_corr, "%.3f");
        ImGui::TableNextColumn();
        if (p.fp_n_samp.has_value()) ImGui::Text("%d", *p.fp_n_samp);
        else ImGui::TextColored(dim, "--");
        ImGui::TableNextColumn();
        if (p.fp_gate_reason.has_value()) ImGui::TextColored(bad, "%s", p.fp_gate_reason->c_str());
        else ImGui::TextColored(dim, "--");
    }
    ImGui::EndTable();
    return clicked_row;
}

// Every column the packet table knows how to plot, in table order -
// used to build both the click-to-graph floating window (one at a
// time) and the "Parameter Grid" tab (all of them at once), off the
// same single source of truth as the table itself.
std::vector<BenchParam> all_plottable_params() {
    std::vector<BenchParam> out;
    for (const auto& c : bench_columns()) {
        if (c.param != BenchParam::None) out.push_back(c.param);
    }
    return out;
}

// x = packet index in chronological order (not wall-clock time - a
// custom test transmission's packets are compared against each other,
// not against absolute time), y = that parameter's value; packets
// where it wasn't computed (gated out, or structurally n/a for that
// modulation) are skipped rather than plotted as a gap-filling zero.
void collect_param_series(const std::vector<BenchPacketPtr>& packets, BenchParam param,
                           std::vector<double>& xs, std::vector<double>& ys) {
    xs.clear();
    ys.clear();
    for (size_t i = 0; i < packets.size(); ++i) {
        auto v = param_value(*packets[i], param);
        if (!v.has_value()) continue;
        xs.push_back(double(i));
        ys.push_back(*v);
    }
}

// One plot, sized to `size` - shared by the click-to-graph floating
// window (full size) and the Parameter Grid tab (card size, many at
// once). `cursor_index`, when >= 0, draws a vertical marker at the
// packet currently shown in the Live tab's top-half plots.
void draw_mini_param_plot(const std::vector<BenchPacketPtr>& packets, BenchParam param,
                           ImVec2 size, int cursor_index, const char* id_suffix) {
    std::vector<double> xs, ys;
    collect_param_series(packets, param, xs, ys);
    ImGui::BeginGroup();
    ImGui::TextUnformatted(param_label(param));
    if (xs.empty()) {
        ImGui::TextDisabled("no data yet");
        ImGui::Dummy(size);
    } else {
        std::string plot_id = std::string("##") + param_label(param) + id_suffix;
        if (ImPlot::BeginPlot(plot_id.c_str(), size, ImPlotFlags_NoLegend)) {
            ImPlot::SetupAxes("Packet #", param_label(param));
            ImPlot::PlotLine(param_label(param), xs.data(), ys.data(), int(xs.size()));
            ImPlot::PlotScatter("##pts", xs.data(), ys.data(), int(xs.size()));
            if (cursor_index >= 0 && cursor_index < int(packets.size())) {
                double cx[2] = {double(cursor_index), double(cursor_index)};
                double cy[2] = {*std::min_element(ys.begin(), ys.end()),
                                 *std::max_element(ys.begin(), ys.end())};
                ImPlot::PlotLine("cursor", cx, cy, 2);
            }
            ImPlot::EndPlot();
        }
    }
    ImGui::EndGroup();
}

// Floating window version of the above - opened by clicking a packet
// table column header (see draw_packet_table()'s sort-click handler).
void draw_param_graph(const std::vector<BenchPacketPtr>& packets, BenchParam param,
                       int cursor_index, bool* p_open) {
    if (param == BenchParam::None || !*p_open) return;
    ImGui::SetNextWindowSize(ImVec2(700, 440), ImGuiCond_FirstUseEver);
    std::string title = std::string(param_label(param)) + " over time";
    if (!ImGui::Begin(title.c_str(), p_open)) {
        ImGui::End();
        return;
    }
    draw_mini_param_plot(packets, param, ImVec2(-1, -1), cursor_index, "float");
    ImGui::End();
}

// --- Parameter Grid tab: every plottable parameter at once, so you
// can see all of them react to a test transmission side by side
// instead of clicking through columns one at a time. ---
void draw_param_grid_view(BenchCapture& capture) {
    std::vector<BenchPacketPtr> packets = capture.packets_snapshot();
    ImGui::TextWrapped(
        "Every numeric parameter from the packet log, plotted against packet # "
        "(chronological order). Send a test transmission on the Live Capture tab to populate this.");
    ImGui::Separator();
    if (packets.empty()) {
        ImGui::TextDisabled("No packets classified yet.");
        return;
    }
    ImGui::BeginChild("bench_param_grid_scroll", ImVec2(0, 0), false);
    constexpr int kColumns = 3;
    float avail_w = ImGui::GetContentRegionAvail().x;
    float card_w = avail_w / float(kColumns) - 10.0f;
    const ImVec2 card_size(card_w, 200.0f);
    auto params = all_plottable_params();
    for (size_t i = 0; i < params.size(); ++i) {
        draw_mini_param_plot(packets, params[i], card_size, /*cursor_index=*/-1, "grid");
        if ((i + 1) % kColumns != 0 && i + 1 != params.size()) ImGui::SameLine();
    }
    ImGui::EndChild();
}

// --- Formulas tab: a static reference grid of every parameter this
// project computes, its formula (plain ASCII - ImGui has no math
// rendering), and its unit. Transcribed from wifi_fingerprint.cpp /
// wifi_phy.cpp; see those files for the actual implementations and the
// reasoning behind each one. Purely descriptive - touches no live state. ---
struct FormulaEntry {
    const char* name;
    const char* unit;
    const char* formula;
    const char* note;
};

const std::vector<FormulaEntry>& formula_reference() {
    static const std::vector<FormulaEntry> entries = {
        {"CFO - coarse (OFDM, Schmidl-Cox)", "Hz, reported as ppm",
         "df = angle(P) * Fs / (2*pi*L)\nP = sum_k conj(x[k]) * x[k+L]",
         "L = 0.8us short-symbol length (L-STF). Unambiguous range +/-625kHz. "
         "ppm = df_total / channel_center_hz * 1e6."},
        {"CFO - fine (OFDM, Moose/L-LTF)", "Hz, reported as ppm",
         "df = angle(P2) * Fs / (2*pi*lag)\nP2 = sum_n conj(d[n]) * d[n+lag]",
         "lag = L-LTF length / 2 (3.2us). Refines the coarse estimate after "
         "derotating by it; unambiguous range +/-156.25kHz."},
        {"CFO (DSSS)", "Hz, reported as ppm",
         "df = angle( sum_k aligned[k+1] * conj(aligned[k]) ) / (2*pi*Tsym)",
         "Tsym = 1us. aligned[k] = despread SYNC symbol with its own DBPSK "
         "modulation re-integrated out first."},
        {"Image Rejection Ratio (IRR)", "dB", "IRR = 20*log10(|nu/mu|)",
         "mu, nu, c from the widely-linear fit r' = mu*s + nu*conj(s) + c against "
         "the L-LTF reference. OFDM only - see IQ eps note."},
        {"IQ gain imbalance (eps)", "unitless (fractional)", "eps = -2 * Re(nu/mu)",
         "0 = perfectly matched I/Q gains. Structurally unresolvable for DSSS: its "
         "real-valued BPSK reference makes s and conj(s) the same vector."},
        {"IQ phase imbalance (phi)", "degrees", "phi = -2 * Im(nu/mu) * 180/pi",
         "0 = perfectly orthogonal I/Q. Same DSSS limitation as IQ eps above."},
        {"DC offset magnitude", "dBc", "DC_dBc = 20*log10(|c| / |mu|)",
         "Relative to the burst's own recovered gain (mu for OFDM, A for DSSS's "
         "reduced 2x2 fit - this one IS identifiable for a real-valued reference)."},
        {"DC offset angle", "degrees", "DC_angle = angle(c/mu) * 180/pi",
         "Phase of the residual LO-leakage/DC term, estimated from the pre-burst "
         "noise window (OFDM) or the fitted DSSS reference."},
        {"SNR (OFDM, true)", "dB", "SNR = 10*log10( (P_burst - P_noise) / P_noise )",
         "P_noise from the raw pre-burst noise window; P_burst from the L-STF/L-LTF "
         "span, both mean |x|^2."},
        {"SNR (DSSS, proxy)", "dB", "SNR = 10*log10( mean(|sym|)^2 / var(|sym|) )",
         "Magnitude-consistency of the despread SYNC symbols - no clean pre-burst "
         "noise sample is available at this stage of the DSSS chain."},
        {"EVM", "%", "EVM = 100 * sqrt( mean(|r' - model|^2) / mean(|s|^2) )",
         "model = mu*s + nu*conj(s) + c (OFDM) or A*s + c (DSSS); r' is the "
         "unit-RMS-normalized, CFO-derotated received signal."},
        {"Sync correlation", "unitless [0,1]", "corr = min(1, |mu| * norm(s) / norm(r'))",
         "How much of the received energy the fitted model actually explains - a "
         "high SNR alone doesn't mean the fit is trustworthy."},
        {"Occupied bandwidth", "Hz (shown as MHz)",
         "span between outermost Welch-PSD bins within 6dB of the peak",
         "Peak-relative, not noise-floor-relative - stays reliable even when a wide, "
         "weak signal already biases the noise floor upward."},
        {"Mean power", "dB (uncalibrated)", "P = 10*log10(mean(|x|^2))",
         "Plain time-domain mean power of the burst window; not calibrated to dBm, "
         "only comparable within one run."},
        {"Confidence", "unitless [0,1]",
         "Barker: mean(|corr|/sqrt(Esig*Etemplate))\nOFDM: mean(|P(d)|^2/R(d)^2)",
         "Mean normalized correlation over whichever correlator's winning peak-"
         "train (DSSS) or plateau run (OFDM) classified the burst."},
        {"N samples (fit)", "count",
         "OFDM: fit_len = 2*lag (L-LTF span)\nDSSS: m = SYNC symbol count",
         "How many samples/symbols the widely-linear fit above was solved over - "
         "context for judging the other fitted quantities, not a quantity itself."},
    };
    return entries;
}

void draw_formulas_view() {
    ImGui::TextWrapped(
        "Every RF parameter this project computes, its formula, and its unit. "
        "Plain ASCII math - ImGui has no LaTeX rendering. See wifi_fingerprint.cpp "
        "and wifi_phy.cpp for the actual implementations these are transcribed from.");
    ImGui::Separator();
    ImGui::BeginChild("bench_formulas_scroll", ImVec2(0, 0), false);
    constexpr int kColumns = 2;
    float avail_w = ImGui::GetContentRegionAvail().x;
    float card_w = avail_w / float(kColumns) - 10.0f;
    const auto& entries = formula_reference();
    for (size_t i = 0; i < entries.size(); ++i) {
        const auto& f = entries[i];
        ImGui::BeginChild((std::string("formula_card_") + std::to_string(i)).c_str(),
                           ImVec2(card_w, 190.0f), true);
        ImGui::TextColored(ImVec4(0.55f, 0.80f, 0.95f, 1.0f), "%s", f.name);
        ImGui::SameLine();
        ImGui::TextDisabled("[%s]", f.unit);
        ImGui::Separator();
        ImGui::TextUnformatted(f.formula);
        ImGui::Spacing();
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + card_w - 16.0f);
        ImGui::TextWrapped("%s", f.note);
        ImGui::PopTextWrapPos();
        ImGui::EndChild();
        if ((i + 1) % kColumns != 0 && i + 1 != entries.size()) ImGui::SameLine();
    }
    ImGui::EndChild();
}

const char* tx_packet_type_label(TxPacketType t) {
    return t == TxPacketType::DSSS ? "DSSS (1Mbps, beacon-shaped)" : "OFDM (L-STF/L-LTF preamble)";
}

// --- Transmit (SMA loopback test) header - see bench_tx.hpp for why
// this exists: send a FIXED, known-good packet out of one USRP channel
// so it can be cabled (through attenuation - see the warning text
// below) straight into the RX side, closing the loop against a signal
// whose ground truth is actually known. Structurally mirrors the RX
// connect header just above it, on purpose. ---
void draw_tx_header(BenchTx& tx, SdrDeviceType selected_device) {
    ImGui::TextUnformatted("Transmit (SMA loopback test)");
    ImGui::TextWrapped(
        "Sends a fixed, known DSSS or OFDM test packet so you can cable TX straight into RX "
        "(through attenuation - USRP TX output can be tens of dB too hot for a direct SMA "
        "connection into an RX input with no attenuator in between) and check the computed "
        "parameters against a signal you actually control, instead of an unknown over-the-air one. "
        "Uses the SDR device selected above.");

    static size_t selected_tx_channel = 0;
    bool tx_running = tx.running();
    ImGui::TextUnformatted("USRP TX channel:");
    ImGui::SameLine();
    for (size_t ch = 0; ch < 2; ++ch) {
        bool sel = selected_tx_channel == ch;
        if (ch > 0) ImGui::SameLine();
        char buf[24];
        std::snprintf(buf, sizeof(buf), "%zu##tx", ch);
        if (ImGui::RadioButton(buf, sel)) selected_tx_channel = ch;
    }
    ImGui::SameLine();
    static float tx_gain_db = 10.0f;
    if (ImGui::Button(tx_running ? "Reconnect##tx" : "Connect TX")) {
        if (tx_running) tx.stop();
        tx.set_device_type(selected_device);
        tx_gain_db = float(device_profile(selected_device).default_gain_db);
        tx.set_gain_db(tx_gain_db);
        tx.start(selected_tx_channel);
    }
    if (tx_running) {
        ImGui::SameLine();
        if (ImGui::Button("Disconnect##tx")) tx.stop();
    }

    BenchTxStatus tx_status = tx.status();
    if (!tx_running) {
        ImGui::TextDisabled("Pick a USRP TX channel above and click Connect TX.");
    } else if (!tx_status.connected) {
        ImGui::TextColored(ImVec4(1, 0.3f, 0.3f, 1), "Not connected%s%s",
                            tx_status.error.empty() ? "" : ": ", tx_status.error.c_str());
    } else {
        ImGui::TextColored(ImVec4(0.25f, 0.73f, 0.31f, 1), "Connected");
        ImGui::SameLine();
        ImGui::Text("| sent: %d", tx_status.send_count);
        if (!tx_status.error.empty()) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1, 0.3f, 0.3f, 1), "| %s", tx_status.error.c_str());
        } else if (!tx_status.last_action.empty()) {
            ImGui::SameLine();
            ImGui::TextDisabled("| %s", tx_status.last_action.c_str());
        }
    }

    static float tx_freq_mhz = 2437.0f;
    ImGui::SetNextItemWidth(140);
    if (ImGui::InputFloat("TX frequency (MHz)##tx", &tx_freq_mhz, 0.0f, 0.0f, "%.4f")) {
        tx.set_freq_hz(double(tx_freq_mhz) * 1e6);
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(lock the Live Capture RX frequency above to this same value)");

    static int packet_type_idx = 0;  // 0 = DSSS, 1 = OFDM
    ImGui::TextUnformatted("Test packet:");
    ImGui::SameLine();
    if (ImGui::RadioButton("DSSS##txtype", &packet_type_idx, 0)) tx.set_packet_type(TxPacketType::DSSS);
    ImGui::SameLine();
    if (ImGui::RadioButton("OFDM##txtype", &packet_type_idx, 1)) tx.set_packet_type(TxPacketType::OFDM);
    ImGui::SameLine();
    ImGui::TextDisabled("(%s)", tx_packet_type_label(packet_type_idx == 0 ? TxPacketType::DSSS : TxPacketType::OFDM));

    ImGui::SetNextItemWidth(160);
    if (ImGui::SliderFloat("TX gain (dB)##tx", &tx_gain_db, 0.0f, float(device_profile(selected_device).max_gain_db), "%.1f")) {
        tx.set_gain_db(tx_gain_db);
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(RX-side gain range reused as a starting point - not separately hardware-validated for TX)");

    ImGui::BeginDisabled(!tx_running);
    if (ImGui::Button("Transmit once")) tx.send_once();
    ImGui::SameLine();
    static bool repeat_enabled = false;
    static float repeat_period_s = 2.0f;
    if (ImGui::Checkbox("Repeat every", &repeat_enabled)) tx.set_repeat(repeat_enabled, double(repeat_period_s));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(100);
    if (ImGui::SliderFloat("s##txrepeat", &repeat_period_s, 0.5f, 10.0f, "%.1f") && repeat_enabled) {
        tx.set_repeat(true, double(repeat_period_s));
    }
    ImGui::EndDisabled();
}

// --- Live Capture tab: connect controls, frequency lock, top-half
// IQ/FFT plots, the packet table, and post-disconnect playback - this
// is everything draw_bench_window used to do before the sidebar/tabs
// were added. ---
void draw_live_view(BenchCapture& capture, BenchTx& tx) {
    // --- SDR device selector - shared by both the RX and TX sides
    // below, same radio-button pattern as main.cpp's own device
    // selector. This is what lets you run TWO instances of this same
    // binary side by side, pick a different device in each, and
    // compare the SAME test transmission's measured parameters across
    // two different pieces of hardware (see bench_capture.hpp's own
    // header). Only takes effect on the next Connect/Connect TX click -
    // switching it while already connected does not hot-swap hardware. ---
    static SdrDeviceType selected_device = SdrDeviceType::X310;
    static const std::vector<std::pair<const char*, SdrDeviceType>> device_options = {
        {"USRP B210", SdrDeviceType::B210},
        {"USRP X310", SdrDeviceType::X310},
    };
    ImGui::TextUnformatted("SDR device:");
    ImGui::SameLine();
    for (size_t i = 0; i < device_options.size(); ++i) {
        bool sel = selected_device == device_options[i].second;
        if (i > 0) ImGui::SameLine();
        if (ImGui::RadioButton(device_options[i].first, sel)) selected_device = device_options[i].second;
    }
    ImGui::Separator();

    // --- USRP RX channel selector + connect ---
    // Channel here means the selected device's own RX frontend - the
    // X310's two independent UBX-160 daughterboard slots, or the
    // B210's two RF chains on its one integrated AD9361 (see
    // config.hpp's DeviceProfile comment) - NOT a Wi-Fi channel
    // number. It is construction-time on UsrpCapture, so changing it
    // (or the device above) while connected requires a
    // disconnect+reconnect, same as main.cpp's own device-type
    // selector.
    static size_t selected_rx_channel = 0;
    bool running = capture.running();
    ImGui::TextUnformatted("USRP RX channel:");
    ImGui::SameLine();
    for (size_t ch = 0; ch < 2; ++ch) {
        bool sel = selected_rx_channel == ch;
        if (ch > 0) ImGui::SameLine();
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%zu", ch);
        if (ImGui::RadioButton(buf, sel)) selected_rx_channel = ch;
    }
    ImGui::SameLine();
    static float gain_db = float(device_profile(SdrDeviceType::X310).default_gain_db);
    if (ImGui::Button(running ? "Reconnect" : "Connect")) {
        if (running) capture.stop();
        capture.set_device_type(selected_device);
        gain_db = float(device_profile(selected_device).default_gain_db);
        capture.set_gain(gain_db);
        capture.start(selected_rx_channel);
    }
    if (running) {
        ImGui::SameLine();
        if (ImGui::Button("Disconnect")) capture.stop();
    }

    BenchStatus status = capture.status();
    if (!running) {
        ImGui::TextDisabled("Pick a USRP RX channel above and click Connect.");
    } else if (!status.connected) {
        ImGui::TextColored(ImVec4(1, 0.3f, 0.3f, 1), "Not connected%s%s",
                            status.error.empty() ? "" : ": ", status.error.c_str());
    } else {
        ImGui::TextColored(ImVec4(0.25f, 0.73f, 0.31f, 1), "Connected");
        ImGui::SameLine();
        ImGui::Text("| chunks: %d | rate: %.3f Msps", status.chunk_count,
                    status.actual_sample_rate_hz / 1e6);
        if (status.last_overflow) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.9f, 0.6f, 0.1f, 1), "| USB/Ethernet overflow last chunk");
        }
        if (status.rx_stalled) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1, 0.3f, 0.3f, 1), "| RX STALLED");
        }
    }
    ImGui::Separator();

    draw_tx_header(tx, selected_device);
    ImGui::Separator();

    // --- Frequency lock ---
    static float freq_mhz = 2437.0f;
    ImGui::SetNextItemWidth(140);
    if (ImGui::InputFloat("Target frequency (MHz)", &freq_mhz, 0.0f, 0.0f, "%.4f")) {
        capture.set_target_freq_hz(double(freq_mhz) * 1e6);
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(USRP actually tuned to %.4f MHz - %.1fMHz DC-guard offset, same as the main app)",
                         (double(freq_mhz) * 1e6 + WIFI_CHANNEL_CAPTURE_OFFSET_HZ) / 1e6,
                         WIFI_CHANNEL_CAPTURE_OFFSET_HZ / 1e6);

    static float threshold_db = 12.0f;
    if (ImGui::SliderFloat("Threshold (dB above noise floor)", &threshold_db, 3.0f, 30.0f, "%.1f")) {
        capture.set_threshold_db(threshold_db);
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(160);
    if (ImGui::SliderFloat("Gain (dB)", &gain_db, 0.0f, float(device_profile(selected_device).max_gain_db), "%.1f")) {
        capture.set_gain(gain_db);
    }
    static float chunk_s = 0.3f;
    ImGui::SetNextItemWidth(160);
    if (ImGui::SliderFloat("Capture chunk (s)", &chunk_s, 0.05f, 1.0f, "%.2f")) {
        capture.set_chunk_duration_s(chunk_s);
    }

    ImGui::Separator();

    std::vector<BenchPacketPtr> packets = capture.packets_snapshot();

    // --- Playback (only meaningful once disconnected - see this file's
    // header) ---
    static bool playing = false;
    static int play_index = 0;
    static float play_rate_hz = 2.0f;
    static double last_step_time = 0.0;
    static int focused_index = -1;  // -1 == "follow latest"

    if (!running && !packets.empty()) {
        ImGui::TextUnformatted("Playback:");
        ImGui::SameLine();
        if (ImGui::Button(playing ? "Pause" : "Play")) {
            playing = !playing;
            last_step_time = ImGui::GetTime();
            if (focused_index < 0) play_index = 0;
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(200);
        int max_index = int(packets.size()) - 1;
        if (ImGui::SliderInt("Packet #", &play_index, 0, max_index)) {
            focused_index = play_index;
            playing = false;
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(120);
        ImGui::SliderFloat("packets/sec", &play_rate_hz, 0.2f, 10.0f, "%.1f");

        if (playing) {
            double now = ImGui::GetTime();
            if (now - last_step_time >= 1.0 / double(play_rate_hz)) {
                last_step_time = now;
                play_index += 1;
                if (play_index > max_index) {
                    play_index = max_index;
                    playing = false;
                }
            }
            focused_index = play_index;
        }
    } else if (running) {
        // A fresh Connect invalidates any prior playback position.
        playing = false;
        focused_index = -1;
    }

    ImGui::Separator();

    // --- Top half: IQ + FFT for whichever packet is "focused" (a
    // manually clicked row, or the playback cursor), falling back to
    // the latest classified packet, and finally to the raw live chunk
    // when nothing has classified yet. ---
    float avail_h = ImGui::GetContentRegionAvail().y;
    float top_h = avail_h * 0.48f;
    int display_index = -1;
    if (focused_index >= 0 && focused_index < int(packets.size())) display_index = focused_index;
    else if (!packets.empty()) display_index = int(packets.size()) - 1;

    if (display_index >= 0) {
        const auto& row = *packets[size_t(display_index)];
        ImGui::Text("Showing packet #%d (seq %llu, %s, %s)", display_index,
                    static_cast<unsigned long long>(row.seq), row.time.c_str(), row.modulation.c_str());
        draw_iq_fft_plots(row.iq, row.sample_rate_hz, row.capture_center_hz, row.segment_center_hz,
                           top_h, "packet");
    } else {
        auto chunk = capture.latest_chunk_snapshot();
        if (chunk) {
            ImGui::TextDisabled("No packet classified yet - showing the live raw capture.");
            draw_iq_fft_plots(chunk->iq, chunk->sample_rate_hz, chunk->capture_center_hz,
                               chunk->segment_center_hz, top_h, "live");
        } else {
            ImGui::TextDisabled("Connect and lock a frequency to see live IQ/FFT.");
            ImGui::Dummy(ImVec2(1, top_h));
        }
    }

    ImGui::Separator();

    // --- Bottom half: packet table, click a row to focus it above,
    // click a numeric column header to graph that parameter over time. ---
    static BenchParam graphed_param = BenchParam::None;
    static bool graph_open = false;
    float bottom_h = ImGui::GetContentRegionAvail().y;
    auto clicked = draw_packet_table(packets, bottom_h, &graphed_param);
    if (clicked.has_value()) {
        focused_index = int(*clicked);
        playing = false;
    }
    if (graphed_param != BenchParam::None) graph_open = true;
    draw_param_graph(packets, graphed_param, display_index, &graph_open);
    if (!graph_open) graphed_param = BenchParam::None;
}

enum class BenchTab { Live, ParamGrid, Formulas };

// Left-hand sidebar - which tab is active persists across frames via
// the caller's static local, matching every other bit of UI state in
// this file.
void draw_sidebar(BenchTab& tab) {
    ImGui::BeginChild("bench_sidebar", ImVec2(180.0f, 0.0f), true);
    struct Item { const char* label; BenchTab tab; };
    static const Item items[] = {
        {"Live Capture", BenchTab::Live},
        {"Parameter Grid", BenchTab::ParamGrid},
        {"Formulas", BenchTab::Formulas},
    };
    for (const auto& it : items) {
        if (ImGui::Selectable(it.label, tab == it.tab, 0, ImVec2(0, 30))) tab = it.tab;
    }
    ImGui::EndChild();
}

}  // namespace

void draw_bench_window(BenchCapture& capture, BenchTx& tx) {
    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::Begin("Wi-Fi Test Bench", nullptr,
                  ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                      ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse);

    static BenchTab tab = BenchTab::Live;
    draw_sidebar(tab);
    ImGui::SameLine();
    ImGui::BeginChild("bench_content", ImVec2(0, 0), false);
    switch (tab) {
        case BenchTab::Live: draw_live_view(capture, tx); break;
        case BenchTab::ParamGrid: draw_param_grid_view(capture); break;
        case BenchTab::Formulas: draw_formulas_view(); break;
    }
    ImGui::EndChild();

    ImGui::End();
}

}  // namespace rfmon::bench
