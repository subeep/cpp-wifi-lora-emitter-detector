#include "process_client.hpp"
#include <chrono>
#include <algorithm>
#include <cerrno>
#include <csignal>
#include <filesystem>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

namespace rfmon::cyclo {
namespace {
ssize_t local_write(int fd, const unsigned char* bytes, std::size_t count) {
    // read/write operate on an already-created local socket. Block SIGPIPE
    // for this call only; never change the application's signal handlers.
    sigset_t set, previous, pending;
    sigemptyset(&set); sigaddset(&set, SIGPIPE);
    if (pthread_sigmask(SIG_BLOCK, &set, &previous)) throw std::runtime_error("cannot protect local IPC write");
    sigpending(&pending); const bool already_pending = sigismember(&pending, SIGPIPE) == 1;
    const auto result = write(fd, bytes, count); const auto saved_errno = errno;
    if (result < 0 && saved_errno == EPIPE && !already_pending) {
        timespec immediate{0, 0}; while (sigtimedwait(&set, nullptr, &immediate) < 0 && errno == EINTR) {}
    }
    pthread_sigmask(SIG_SETMASK, &previous, nullptr); errno = saved_errno;
    return result;
}
std::string default_worker_path() {
    char path[4096]; const auto count = readlink("/proc/self/exe", path, sizeof(path));
    if (count <= 0 || std::size_t(count) >= sizeof(path)) throw WorkerError(WorkerFault::transport, "cannot resolve worker executable");
    const auto directory = std::filesystem::path(std::string(path, std::size_t(count))).parent_path();
    for (const auto& candidate : {directory / "wifi_cyclo_worker", directory / "tools/cyclostationary/wifi_cyclo_worker"})
        if (std::filesystem::is_regular_file(candidate)) return candidate.string();
    throw WorkerError(WorkerFault::transport, "cyclostationary worker executable is missing");
}
}
WireBytes worker_hello_payload() {
    // Advertised address-space bytes and CPU lifetime seconds; parent requires
    // this exact protocol profile. Parent additionally owns wall deadlines.
    auto p = encode_error(128ull * 1024 * 1024), cpu = encode_error(30);
    p.insert(p.end(), cpu.begin(), cpu.end()); return p;
}
ProcessClient::ProcessClient(WorkerOptions options) : options_(std::move(options)) {
    if (options_.deadline_ms < 50 || options_.deadline_ms > 10000 || options_.restart_limit > 8)
        throw std::invalid_argument("invalid cyclostationary worker supervision limits");
}
ProcessClient::~ProcessClient() { terminate(); }

void ProcessClient::start(const std::function<bool()>& cancelled) {
    if (running()) return;
    if (cancelled()) throw WorkerError(WorkerFault::cancelled, "analysis cancelled");
    const auto path = options_.executable.empty() ? default_worker_path() : options_.executable;
    if (!std::filesystem::path(path).is_absolute()) throw WorkerError(WorkerFault::transport, "worker path must be absolute");
    int sockets[2];
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets)) throw WorkerError(WorkerFault::transport, "worker socket creation failed");
    posix_spawn_file_actions_t actions;
    const int init = posix_spawn_file_actions_init(&actions);
    int error = init;
    // Close parent end before dup2; it may occupy descriptor 3. Close all
    // unrelated descriptors: no inherited SDR, log, UI or network handles.
    if (!error) error = posix_spawn_file_actions_addclose(&actions, sockets[0]);
    if (!error) error = posix_spawn_file_actions_adddup2(&actions, sockets[1], 3);
    if (!error) error = posix_spawn_file_actions_addclosefrom_np(&actions, 4);
    for (int fd = 0; fd < 3 && !error; ++fd)
        error = posix_spawn_file_actions_addopen(&actions, fd, "/dev/null", fd == 0 ? O_RDONLY : O_WRONLY, 0);
    char fd_arg[] = "3", option[] = "--ipc-fd";
    char parent_option[] = "--parent-pid";
    const auto parent_pid = std::to_string(getpid());
    char* argv[] = {const_cast<char*>(path.c_str()), option, fd_arg, parent_option,
                    const_cast<char*>(parent_pid.c_str()), nullptr};
    char locale[] = "LC_ALL=C", system_path[] = "PATH=/usr/bin:/bin";
    char* environment[] = {locale, system_path, nullptr};
    pid_t pid = -1;
    if (!error) error = posix_spawn(&pid, path.c_str(), &actions, nullptr, argv, environment);
    if (!init) posix_spawn_file_actions_destroy(&actions);
    close(sockets[1]);
    if (error) { close(sockets[0]); throw WorkerError(WorkerFault::transport, "worker spawn failed"); }
    fd_ = sockets[0]; pid_ = pid;
    if (fcntl(fd_, F_SETFL, O_NONBLOCK) < 0) { terminate(); throw WorkerError(WorkerFault::transport, "worker nonblocking socket setup failed"); }
    try {
        const auto hello = receive(std::chrono::steady_clock::now() + std::chrono::milliseconds(options_.deadline_ms), cancelled);
        if (hello.first != MessageType::hello || hello.second != worker_hello_payload())
            throw WorkerError(WorkerFault::protocol, "incompatible worker handshake");
    } catch (...) { terminate(); throw; }
}

void ProcessClient::terminate() noexcept {
    if (fd_ >= 0) { close(fd_); fd_ = -1; }
    if (pid_ > 0) {
        // Reap an already exited child first, so a PID that is no longer ours
        // is never signalled. No process groups or unrelated PIDs are touched.
        pid_t reaped;
        do { reaped = waitpid(pid_, nullptr, WNOHANG); } while (reaped < 0 && errno == EINTR);
        if (reaped == 0) {
            kill(pid_, SIGKILL);
            const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
            while (std::chrono::steady_clock::now() < end) {
                const auto rc = waitpid(pid_, nullptr, WNOHANG);
                if (rc == pid_ || (rc < 0 && errno == ECHILD)) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        }
        pid_ = -1;
    }
}

void ProcessClient::transfer(unsigned char* bytes, std::size_t count, bool sending,
    std::chrono::steady_clock::time_point deadline, const std::function<bool()>& cancelled) {
    std::size_t done = 0;
    while (done < count) {
        if (cancelled()) throw WorkerError(WorkerFault::cancelled, "analysis cancelled");
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
        if (remaining <= 0) throw WorkerError(WorkerFault::timeout, "worker deadline exceeded");
        pollfd p{fd_, short(sending ? POLLOUT : POLLIN), 0};
        const auto rc = poll(&p, 1, int(std::min<std::int64_t>(remaining, 20)));
        if (rc < 0) { if (errno == EINTR) continue; throw WorkerError(WorkerFault::transport, "worker poll failed"); }
        if (!rc) continue;
        const auto n = sending ? local_write(fd_, bytes + done, count - done)
                               : read(fd_, bytes + done, count - done);
        if (n <= 0) {
            if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
            throw WorkerError(WorkerFault::transport, "worker disconnected");
        }
        done += std::size_t(n);
    }
}
std::pair<MessageType, WireBytes> ProcessClient::receive(std::chrono::steady_clock::time_point deadline,
                                                       const std::function<bool()>& cancelled) {
    WireBytes header(16); transfer(header.data(), header.size(), false, deadline, cancelled);
    WireHeader h;
    try { h = decode_header(header, reply_byte_limit); }
    catch (...) { throw WorkerError(WorkerFault::protocol, "invalid worker reply header"); }
    WireBytes payload(h.bytes); transfer(payload.data(), payload.size(), false, deadline, cancelled);
    return {h.type, std::move(payload)};
}
std::vector<TileMeasurement> ProcessClient::analyze(std::uint64_t id, double rate, const TilePlan& plan,
    const std::vector<std::complex<float>>& iq, const std::function<bool()>& cancelled) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(options_.deadline_ms);
    auto payload = encode_request(id, rate, plan, iq), header = encode_header(MessageType::request, payload.size());
    transfer(header.data(), header.size(), true, deadline, cancelled);
    transfer(payload.data(), payload.size(), true, deadline, cancelled);
    auto reply = receive(deadline, cancelled);
    try {
        if (reply.first == MessageType::error) {
            validate_error(reply.second, id); throw WorkerError(WorkerFault::analysis, "worker rejected measurement input");
        }
        if (reply.first != MessageType::result) throw std::runtime_error("unexpected reply type");
        return decode_result(reply.second, id, rate, plan);
    } catch (const WorkerError&) { throw; }
    catch (...) { throw WorkerError(WorkerFault::protocol, "invalid worker measurement payload"); }
}

bool read_worker_frame(int fd, std::size_t limit, MessageType& type, WireBytes& payload) {
    auto read_all = [&](WireBytes& bytes, bool allow_eof) {
        std::size_t at = 0;
        while (at < bytes.size()) {
            const auto n = read(fd, bytes.data() + at, bytes.size() - at);
            if (n < 0 && errno == EINTR) continue;
            if (!n && !at && allow_eof) return false;
            if (n <= 0) throw std::runtime_error("worker input ended mid-frame");
            at += std::size_t(n);
        }
        return true;
    };
    WireBytes header(16); if (!read_all(header, true)) return false;
    const auto h = decode_header(header, limit); type = h.type; payload.resize(h.bytes);
    read_all(payload, false); return true;
}
void write_worker_frame(int fd, MessageType type, const WireBytes& payload) {
    const auto header = encode_header(type, payload.size());
    for (const auto* bytes : {&header, &payload}) {
        std::size_t at = 0;
        while (at < bytes->size()) {
            const auto n = local_write(fd, bytes->data() + at, bytes->size() - at);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) throw std::runtime_error("worker output failed");
            at += std::size_t(n);
        }
    }
}
} // namespace rfmon::cyclo
