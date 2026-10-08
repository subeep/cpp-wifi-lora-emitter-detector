#include "cyclostationary/process_client.hpp"
#include <cerrno>
#include <charconv>
#include <csignal>
#include <string>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <unistd.h>

using namespace rfmon::cyclo;
int main(int argc, char** argv) {
    if (argc != 5 || std::string(argv[1]) != "--ipc-fd" || std::string(argv[2]) != "3" ||
        std::string(argv[3]) != "--parent-pid") return 2;
    int expected_parent = 0; const auto text = std::string(argv[4]);
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), expected_parent);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || expected_parent <= 0) return 2;
    if (prctl(PR_SET_PDEATHSIG, SIGKILL) || getppid() != expected_parent) return 3;
    // Applied in the child only, after exec's shared libraries have loaded.
    rlimit memory{128ull * 1024 * 1024, 128ull * 1024 * 1024}, cpu{30, 30}, files{32, 32}, core{0, 0};
    errno = 0; const int priority = getpriority(PRIO_PROCESS, 0);
    const bool priority_valid = errno == 0;
    if (setrlimit(RLIMIT_AS, &memory) || setrlimit(RLIMIT_CPU, &cpu) || setrlimit(RLIMIT_NOFILE, &files) ||
        setrlimit(RLIMIT_CORE, &core) || prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) ||
        !priority_valid || (priority < 10 && setpriority(PRIO_PROCESS, 0, 10))) return 3;
    try {
        write_worker_frame(3, MessageType::hello, worker_hello_payload());
        MessageType type; WireBytes payload;
        while (read_worker_frame(3, request_byte_limit, type, payload)) {
            if (type != MessageType::request) return 4;
            const auto request = decode_request(payload);
            try {
                std::vector<TileMeasurement> results;
                for (const auto& input : request.tiles) {
                    TileMeasurement r; r.source = input.source;
                    SpectralConfig c; c.sample_rate_hz = request.rate_hz; c.max_frames = 256;
                    r.spectral = measure_spectral_correlation(input.iq, c);
                    r.ofdm = measure_ofdm_structure(input.iq, request.rate_hz, wlan_ofdm_hypotheses(request.rate_hz));
                    r.roi = measure_rois(input.iq, request.rate_hz);
                    r.chirps = measure_chirp_structure(input.iq, request.rate_hz);
                    r.waveform = measure_waveform_features(input.iq, request.rate_hz);
                    r.cyclic_background = measure_cyclic_background(input.iq,request.rate_hz,r.waveform);
                    r.structure = discover_waveform_structure(input.iq, request.rate_hz);
                    r.sweeps = discover_linear_sweeps(input.iq, request.rate_hz);
                    r.burst=analyze_burst(input.iq,request.rate_hz,r.roi);
                    if(r.burst.selection.samples) {
                        const auto begin=input.iq.begin()+r.burst.selection.offset;
                        r.clock=refine_structure_clock({begin,begin+r.burst.selection.samples},request.rate_hz,r.burst.structure);
                    } else r.clock=refine_structure_clock(input.iq,request.rate_hz,r.structure);
                    results.push_back(std::move(r));
                }
                write_worker_frame(3, MessageType::result, encode_result(request.job_id, results));
            } catch (const std::exception&) {
                write_worker_frame(3, MessageType::error, encode_error(request.job_id));
            }
        }
    } catch (...) { return 5; }
}
