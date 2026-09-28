#include "timer.h"

#include <algorithm>
#include <condition_variable>
#include <iterator>

namespace {
// Longest single sleep inside one tick, so timer_stop() (e.g. on plugin
// unload) returns within this bound instead of after a whole interval.
constexpr std::chrono::milliseconds kStopPollSlice(200);

// Which callback group each Timer is currently invoking. Kept outside the
// class because Timer's layout is part of the plugin ABI (timer.h).
// remove_callback() waits on this so the framework can dlclose a plugin
// right after removing its callback: nothing from that .so (the call itself
// or the std::function copies' destructors) runs once it returns.
struct InFlightEntry {
    std::string name;
    std::thread::id tid;
};
struct InFlightRegistry {
    std::mutex m;
    std::condition_variable cv;
    std::map<const Timer *, InFlightEntry> running;
};
InFlightRegistry &in_flight() {
    // Leaked on purpose: timer threads may still be unwinding at exit.
    static auto *reg = new InFlightRegistry();
    return *reg;
}

// Clears the in-flight mark on scope exit (also if a handler throws).
class InFlightGuard {
public:
    explicit InFlightGuard(const Timer *t) : timer_(t) {}
    InFlightGuard(const InFlightGuard &) = delete;
    InFlightGuard &operator=(const InFlightGuard &) = delete;
    void mark(const std::string &name) {
        InFlightRegistry &reg = in_flight();
        std::lock_guard<std::mutex> lock(reg.m);
        reg.running[timer_] = InFlightEntry{name, std::this_thread::get_id()};
        active_ = true;
    }
    ~InFlightGuard() {
        if (!active_) {
            return;
        }
        InFlightRegistry &reg = in_flight();
        {
            std::lock_guard<std::mutex> lock(reg.m);
            reg.running.erase(timer_);
        }
        reg.cv.notify_all();
    }

private:
    const Timer *timer_;
    bool active_ = false;
};
} // namespace

void Timer::run() {
    while (running.load()) {
        std::chrono::duration<double> dur;
        {
            std::lock_guard<std::mutex> lock(callbacks_mutex);
            dur = interval;
        }
        const auto deadline =
            std::chrono::steady_clock::now() +
            std::chrono::duration_cast<std::chrono::steady_clock::duration>(dur);
        while (running.load()) {
            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline) {
                break;
            }
            std::this_thread::sleep_for(
                std::min<std::chrono::steady_clock::duration>(deadline - now,
                                                              kStopPollSlice));
        }
        if (running.load()) {
            std::vector<std::string> names;
            {
                std::lock_guard<std::mutex> lock(callbacks_mutex);
                names.reserve(callbacks.size());
                std::transform(callbacks.begin(), callbacks.end(),
                               std::back_inserter(names),
                               [](const auto &u) { return u.first; });
            }

            for (const auto &name : names) {
                // guard is declared before fns, so the std::function copies
                // (whose code lives in the plugin) die while still marked.
                InFlightGuard guard(this);
                std::vector<std::function<void(bot * p)>> fns;
                {
                    std::lock_guard<std::mutex> lock(callbacks_mutex);
                    auto it = callbacks.find(name);
                    if (it == callbacks.end()) {
                        continue; // removed earlier in this tick
                    }
                    fns = it->second;
                    // marked under callbacks_mutex: a concurrent
                    // remove_callback either erased it first (skipped above)
                    // or will see the mark and wait
                    guard.mark(name);
                }
                // Log first, then tell the ops. cq_send throws when the
                // backend is unreachable; letting that escape the timer
                // thread would std::terminate the whole bot.
                const auto report = [this](const std::string &text) {
                    p->setlog(LOG::ERROR, text);
                    try {
                        p->cq_send_all_op(text);
                    } catch (...) {
                        p->setlog(LOG::WARNING,
                                  "Timer: could not notify ops (backend down?)");
                    }
                };
                for (const auto &f : fns) {
                    try {
                        f(this->p);
                    } catch (const char *e) {
                        report(std::string("Timer Throw an char*: ") + e);
                    } catch (const std::string &e) {
                        report("Timer Throw an string: " + e);
                    } catch (const std::exception &e) {
                        report(std::string("Timer Throw an exception: ") +
                               e.what());
                    } catch (...) {
                        report("Timer Throw an unknown error");
                    }
                }
            }
        }
    }
}

Timer::Timer(std::chrono::duration<double> dur, bot *p)
    : interval(dur), running(false), p(p) {}

void Timer::set_interval(std::chrono::duration<double> dur) {
    // run() reads interval from the timer thread; share callbacks_mutex.
    std::lock_guard<std::mutex> lock(callbacks_mutex);
    interval = dur;
}
void Timer::add_callback(const std::string &name,
                         std::function<void(bot *p)> cb) {
    std::lock_guard<std::mutex> lock(callbacks_mutex);
    this->callbacks[name].push_back(std::move(cb));
}
// Blocks until an in-flight invocation of `name` has returned (unless called
// from that very callback), so the caller may unload the plugin afterwards.
// Don't call it while holding a lock the callback itself may need.
void Timer::remove_callback(const std::string &name) {
    {
        std::lock_guard<std::mutex> lock(callbacks_mutex);
        this->callbacks.erase(name);
    }
    InFlightRegistry &reg = in_flight();
    std::unique_lock<std::mutex> lock(reg.m);
    reg.cv.wait(lock, [&] {
        auto it = reg.running.find(this);
        return it == reg.running.end() || it->second.name != name ||
               it->second.tid == std::this_thread::get_id();
    });
}

void Timer::timer_start() {
    if (timerThread.joinable()) {
        if (running.load()) {
            return; // already running; re-assigning a joinable std::thread
                    // would std::terminate the process
        }
        timerThread.join(); // stopped from inside a callback, not yet joined
    }
    running.store(true);
    timerThread = std::thread(&Timer::run, this);
}

void Timer::timer_stop() {
    running.store(false);
    if (timerThread.joinable()) {
        if (timerThread.get_id() == std::this_thread::get_id()) {
            return; // called from a callback: run() exits after it returns;
                    // joining ourselves would throw
        }
        timerThread.join();
    }
}

Timer::~Timer() {
    timer_stop();
    if (timerThread.joinable()) {
        // Destroyed from its own callback: the thread cannot be joined here,
        // and a joinable std::thread destructor would std::terminate.
        timerThread.detach();
    }
}
