// Deliberately broken local child programs; never linked into the application.
#include "cyclostationary/process_client.hpp"
#include <csignal>
#include <sys/resource.h>
#include <sys/socket.h>
#include <unistd.h>
using namespace rfmon::cyclo;
int main() {
    rlimit core{0, 0}; setrlimit(RLIMIT_CORE, &core);
    write_worker_frame(3, MessageType::hello, worker_hello_payload());
    MessageType type; WireBytes request;
    if (!read_worker_frame(3, request_byte_limit, type, request)) return 2;
#if CYCLO_FAULT_MODE == 1
    raise(SIGSEGV);
#elif CYCLO_FAULT_MODE == 2
    for (;;) pause();
#elif CYCLO_FAULT_MODE == 3
    const auto header = encode_header(MessageType::result, reply_byte_limit + 1);
    if (write(3, header.data(), header.size()) != ssize_t(header.size())) return 3;
    for (;;) pause();
#elif CYCLO_FAULT_MODE == 4
    write_worker_frame(3, MessageType::result, WireBytes{1, 2, 3});
    for (;;) pause();
#endif
}
