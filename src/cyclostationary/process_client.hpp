#pragma once
#include "worker_protocol.hpp"
#include <functional>
#include <chrono>
#include <string>

namespace rfmon::cyclo {
struct WorkerOptions {
    std::string executable; // empty: resolve beside the running executable
    int deadline_ms = 2000;
    unsigned restart_limit = 3;
};
enum class WorkerFault { cancelled, timeout, transport, protocol, analysis };
class WorkerError : public std::runtime_error {
public:
    WorkerError(WorkerFault fault, const char* message) : std::runtime_error(message), fault(fault) {}
    WorkerFault fault;
};
// Supervisor-side IPC. Used only by the auxiliary thread, never acquisition.
class ProcessClient {
public:
    explicit ProcessClient(WorkerOptions options);
    ~ProcessClient();
    ProcessClient(const ProcessClient&) = delete;
    ProcessClient& operator=(const ProcessClient&) = delete;
    bool running() const noexcept { return fd_ >= 0; }
    int pid() const noexcept { return pid_; }
    void start(const std::function<bool()>& cancelled);
    void terminate() noexcept;
    std::vector<TileMeasurement> analyze(std::uint64_t job_id, double rate, const TilePlan& plan,
        const std::vector<std::complex<float>>& iq, const std::function<bool()>& cancelled);
private:
    void transfer(unsigned char* bytes, std::size_t count, bool sending,
                  std::chrono::steady_clock::time_point deadline, const std::function<bool()>& cancelled);
    std::pair<MessageType, WireBytes> receive(std::chrono::steady_clock::time_point deadline,
                                             const std::function<bool()>& cancelled);
    WorkerOptions options_;
    int fd_ = -1, pid_ = -1;
};

// Child-side blocking I/O. Only the parent's inherited local socket is used.
bool read_worker_frame(int fd, std::size_t limit, MessageType& type, WireBytes& payload);
void write_worker_frame(int fd, MessageType type, const WireBytes& payload);
WireBytes worker_hello_payload();
} // namespace rfmon::cyclo
