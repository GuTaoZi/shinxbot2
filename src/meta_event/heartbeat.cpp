#include "heartbeat.h"

#include <cerrno>
#include <cstdlib>
#include <unistd.h>

static inline std::time_t get_current_time() {
    auto now = std::chrono::system_clock::now();
    return std::chrono::system_clock::to_time_t(now);
}

// The recovery child is reaped here, on the heartbeat thread, rather than by a
// detached waiter thread, so no thread outlives the heartBeat object.
void heartBeat::reap_recover() {
    if (recover_pid_ <= 0) {
        return;
    }
    pid_t r;
    do {
        r = waitpid(recover_pid_, nullptr, WNOHANG);
    } while (r == -1 && errno == EINTR);
    if (r != 0) { // reaped, or no longer our child
        recover_pid_ = -1;
    }
}

void heartBeat::start_recover() {
    reap_recover();
    if (recover_pid_ > 0) { // previous recovery still running
        return;
    }
    pid_t k = fork();
    if (k == -1) {
        std::cerr << "Process Error!" << std::endl;
    } else if (k == 0) {
        for (const std::string &u : recover_commands) {
            // NOLINTNEXTLINE(concurrency-mt-unsafe): single-threaded child
            int rc = system(u.c_str());
            if (rc == -1) {
                std::cerr << "recover command failed to launch: " << u
                          << std::endl;
            }
        }
        _exit(0); // don't run the parent's atexit handlers in the child
    } else {
        recover_pid_ = k;
    }
}
heartBeat::heartBeat(const std::vector<std::string> &commands)
    : recover_commands(commands), las_time(get_current_time()) {}

void heartBeat::run() {
    while (running_.load()) {
        reap_recover();
        if (is_recorded.load() &&
            get_current_time() - las_time.load() >= 3600 * 8 &&
            !recovering.load()) { // 8h
            start_recover();
            recovering.store(true);
        }
        std::unique_lock<std::mutex> lk(stop_mutex_);
        stop_cv_.wait_for(lk, std::chrono::seconds(60),
                          [this] { return !running_.load(); });
    }
    reap_recover();
}

void heartBeat::stop() {
    {
        std::lock_guard<std::mutex> lk(stop_mutex_);
        running_.store(false);
    }
    stop_cv_.notify_all();
}

void heartBeat::inform() {
    las_time.store(get_current_time());
    is_recorded.store(true);
    recovering.store(false);
}
