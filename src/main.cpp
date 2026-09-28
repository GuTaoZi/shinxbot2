#include "shinxbot.hpp"
#include "utils.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <sys/wait.h>
#include <thread>
#include <vector>

#include <Magick++.h>

int send_port, receive_port;
bot *bots;

namespace {

void log_child_exit(pid_t pid, int status, std::chrono::seconds uptime) {
    std::cerr << "[bot_run] child " << pid << " ";
    if (WIFEXITED(status)) {
        std::cerr << "exited with code " << WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        std::cerr << "killed by signal " << WTERMSIG(status) << " ("
                  // NOLINTNEXTLINE(concurrency-mt-unsafe): parent is 1 thread
                  << strsignal(WTERMSIG(status)) << ")";
    } else {
        std::cerr << "stopped, status " << status;
    }
    std::cerr << " after " << uptime.count() << "s" << std::endl;
}

} // namespace

// Fork-loop supervisor: re-forks the bot whenever the child dies. A child that
// dies soon after start (bad config, port in use, crash in init) backs off
// exponentially (1s .. 60s) instead of re-forking in a tight loop; a child that
// ran for a while restarts immediately, as before.
void bot_run(bot *u) {
    using clock = std::chrono::steady_clock;
    constexpr std::chrono::seconds kHealthyUptime{60};
    constexpr std::chrono::seconds kMaxBackoff{60};
    std::chrono::seconds backoff{0};

    while (true) {
        if (backoff.count() > 0) {
            std::cerr << "[bot_run] restarting in " << backoff.count() << "s"
                      << std::endl;
            std::this_thread::sleep_for(backoff);
        }
        const auto started = clock::now();
        pid_t k = fork();
        if (k == -1) {
            std::cerr << "Process Error! fork: "
                      << std::strerror(errno) // NOLINT(concurrency-mt-unsafe)
                      << std::endl;
            backoff = std::clamp(backoff * 2, std::chrono::seconds{1},
                                 kMaxBackoff);
            continue;
        }
        if (k == 0) {
            u->run();
            // Plugin/timer threads may still be running: skip static
            // destructors (they'd race those threads) and just flush + leave.
            std::cout.flush();
            std::cerr.flush();
            _exit(0);
        }

        int status = 0;
        pid_t r;
        do {
            r = waitpid(k, &status, 0);
        } while (r == -1 && errno == EINTR);
        const auto uptime = std::chrono::duration_cast<std::chrono::seconds>(
            clock::now() - started);
        if (r == k) {
            log_child_exit(k, status, uptime);
        }
        if (uptime >= kHealthyUptime) {
            backoff = std::chrono::seconds{0};
        } else {
            backoff = std::clamp(backoff * 2, std::chrono::seconds{1},
                                 kMaxBackoff);
        }
    }
}

int main() {
    // Lock bot-wide local time to Beijing time (daily resets, date-based
    // plugins, log rotation) regardless of the host's timezone. Done before
    // any thread exists, so setenv/tzset here are safe.
    setenv("TZ", "Asia/Shanghai", 1); // NOLINT(concurrency-mt-unsafe)
    tzset();
    Magick::InitializeMagick("shinxBot");
    if (std::signal(SIGPIPE, SIG_IGN) == SIG_ERR ||
        std::signal(SIGALRM, SIG_IGN) == SIG_ERR) {
        std::cerr << "warning: failed to ignore SIGPIPE/SIGALRM" << std::endl;
    }
    std::ifstream iport("./config/port.txt");
    int x = 0, y = 0;
    std::string token;
    if (iport.is_open()) {
        // one line: "<send> <recv> <token>" (token may be absent)
        if (!(iport >> x >> y)) {
            std::cerr << "./config/port.txt is malformed; expected one line: "
                         "\"<send_port> <recv_port> <token>\""
                      << std::endl;
            return 1;
        }
        iport >> token;
        iport.close();
    } else {
        std::cout << "Please input the send_port: (receive port in Onebot11):";
        std::cin >> x;
        std::cout << "Please input the receive_port: (send port in Onebot11):";
        std::cin >> y;
        std::cout << "Please input the token:";
        std::cin >> token;
        std::ofstream oport("./config/port.txt");
        if (oport) {
            oport << x << ' ' << y << ' ' << token;
            oport.flush();
            oport.close();
        }
    }

    bot_run(bots = new shinxbot(y, x, token));

    // Never goes here~ (bot_run loops forever)
    delete bots;

    Magick::TerminateMagick();

    return 0;
}
