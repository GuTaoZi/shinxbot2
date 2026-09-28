#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <ctime>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <string>
#include <sys/types.h>
#include <sys/wait.h>
#include <thread>
#include <vector>

class heartBeat {
private:
    std::vector<std::string> recover_commands;

    // inform() runs on the HTTP worker threads, run() on the heartbeat thread.
    std::atomic<bool> is_recorded{false};
    std::atomic<bool> recovering{true};
    std::atomic<std::time_t> las_time;
    std::atomic<bool> running_{true};

    // Lets stop() wake run() immediately instead of after up to 60s.
    std::mutex stop_mutex_;
    std::condition_variable stop_cv_;
    pid_t recover_pid_ = -1; // only touched by the heartbeat thread

    void start_recover();
    void reap_recover();

public:
    explicit heartBeat(const std::vector<std::string> &commands);
    heartBeat() = delete;
    heartBeat(heartBeat &) = delete;
    heartBeat(heartBeat &&) = delete;
    heartBeat operator=(heartBeat) = delete;
    void run();
    void stop();
    void inform();
};
