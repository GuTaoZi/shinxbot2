#pragma once

#include "eventprocess.h"
#include "heartbeat.h"
#include "processable.h"

#include <algorithm>
#include <atomic>
#include <dlfcn.h>
#include <fstream>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <thread>
#include <tuple>
#include <unistd.h>
#include <vector>

// Destroy a plugin object through its own destroy_t, then dlclose it. A
// throwing plugin destructor is logged and must not skip the dlclose.
template <typename T> void close_dl(void *handle, T *p) {
    typedef void (*close_t)(T *);
    dlerror(); // clear stale state so the check below is meaningful
    close_t closex = reinterpret_cast<close_t>(dlsym(handle, "destroy_t"));
    const char *dlsym_error = dlerror();
    if (dlsym_error) {
        set_global_log(LOG::WARNING,
                       std::string("Cannot load symbol 'destroy_t': ") +
                           dlsym_error);
        // delete p; // This is not always safe
    } else {
        try {
            closex(p);
        } catch (const std::exception &e) {
            set_global_log(LOG::ERROR,
                           std::string("destroy_t threw: ") + e.what());
        } catch (...) {
            set_global_log(LOG::ERROR, "destroy_t threw unknown error");
        }
    }
    dlclose(handle);
}

// One loaded plugin. Entries don't own it by default (run()'s teardown still
// deletes/dlcloses them directly); once a module is unloaded at runtime it is
// marked close_on_release and closed when the last dispatch pass still holding
// it lets go -- so it is never dlclosed under a running check()/process().
// No lock is held across plugin calls: plugins re-enter input_process().
template <typename T> struct module_owner {
    T *obj;
    void *handle;
    std::atomic<bool> close_on_release{false};

    module_owner(T *o, void *h) : obj(o), handle(h) {}
    module_owner(const module_owner &) = delete;
    module_owner &operator=(const module_owner &) = delete;
    ~module_owner() {
        if (close_on_release.load()) {
            close_dl(handle, obj);
        }
    }
};

// (object, dl handle, alias) -- still a std::tuple so std::get<> works as
// before, plus the shared owner that keeps the module mapped.
template <typename T> struct module_ref : std::tuple<T *, void *, std::string> {
    std::shared_ptr<module_owner<T>> owner;

    module_ref(T *obj, void *handle, const std::string &name)
        : std::tuple<T *, void *, std::string>(obj, handle, name),
          owner(std::make_shared<module_owner<T>>(obj, handle)) {}
    const std::string &name() const { return std::get<2>(*this); }
};

// Copy-on-write list of loaded modules. Iterating takes one snapshot (a single
// shared_ptr copy under a short lock) that stays valid for the whole loop, even
// if a module is loaded/unloaded concurrently or from inside the loop.
template <typename T> class module_registry {
public:
    using entry = module_ref<T>;
    using list = std::vector<entry>;

    class sentinel {};
    class iterator {
    private:
        std::shared_ptr<const list> snap_;
        size_t i_ = 0;

    public:
        explicit iterator(std::shared_ptr<const list> s)
            : snap_(std::move(s)) {}
        const entry &operator*() const { return (*snap_)[i_]; }
        const entry *operator->() const { return &(*snap_)[i_]; }
        iterator &operator++() {
            ++i_;
            return *this;
        }
        bool operator!=(const sentinel &) const { return i_ < snap_->size(); }
    };

    iterator begin() const { return iterator(snapshot()); }
    static sentinel end() { return {}; }

    std::shared_ptr<const list> snapshot() const {
        std::lock_guard<std::mutex> lock(mu_);
        return cur_;
    }
    bool empty() const { return snapshot()->empty(); }
    bool contains(const std::string &name) const {
        const auto snap = snapshot();
        return std::any_of(snap->begin(), snap->end(),
                           [&](const entry &e) { return e.name() == name; });
    }

    void push_back(const std::tuple<T *, void *, std::string> &t) {
        insert(npos, std::get<0>(t), std::get<1>(t), std::get<2>(t));
    }
    // Insert at pos (clamped; npos = append), keeping dispatch order stable.
    void insert(size_t pos, T *obj, void *handle, const std::string &name) {
        mutate([&](list &l) {
            pos = std::min(pos, l.size());
            l.insert(l.begin() + static_cast<std::ptrdiff_t>(pos),
                     entry(obj, handle, name));
        });
    }
    // Remove `name`; returns its owner (null if absent) and former position.
    std::pair<std::shared_ptr<module_owner<T>>, size_t>
    take(const std::string &name) {
        std::pair<std::shared_ptr<module_owner<T>>, size_t> out{nullptr, npos};
        mutate([&](list &l) {
            for (size_t i = 0; i < l.size(); ++i) {
                if (l[i].name() == name) {
                    out = {l[i].owner, i};
                    l.erase(l.begin() + static_cast<std::ptrdiff_t>(i));
                    return;
                }
            }
        });
        return out;
    }
    void clear() {
        std::lock_guard<std::mutex> lock(mu_);
        cur_ = std::make_shared<const list>();
    }
    // Mark every module for closing and drop them (each is closed as soon as
    // no dispatch pass holds it).
    void close_all() {
        std::shared_ptr<const list> old;
        {
            std::lock_guard<std::mutex> lock(mu_);
            old.swap(cur_);
            cur_ = std::make_shared<const list>();
        }
        for (const auto &e : *old) {
            e.owner->close_on_release = true;
        }
    }

    static constexpr size_t npos = static_cast<size_t>(-1);

private:
    template <typename F> void mutate(F &&f) {
        std::lock_guard<std::mutex> lock(mu_);
        auto next = std::make_shared<list>(*cur_);
        f(*next);
        cur_ = std::move(next);
    }

    mutable std::mutex mu_;
    std::shared_ptr<const list> cur_ = std::make_shared<const list>();
};

class blockItem {
private:
    std::set<std::string> blocklist;
    std::set<std::string> whitelist;
    bool mode; // true: blocklist, false: whitelist
public:
    blockItem() : mode(true) {}
    blockItem(const std::set<std::string> &blocklist,
              const std::set<std::string> &whitelist, bool mode)
        : blocklist(blocklist), whitelist(whitelist), mode(mode) {}
    explicit blockItem(const Json::Value &J) : mode(true) {
        if (J.isMember("block") && J.isMember("white") && J.isMember("mode")) {
            parse_json_to_set(J["block"], blocklist);
            parse_json_to_set(J["white"], whitelist);
            mode = J["mode"].asBool();
        }
    }
    bool is_blocked(const std::string &message) const {
        if (mode) {
            return blocklist.find(message) != blocklist.end();
        } else {
            return whitelist.find(message) == whitelist.end();
        }
    }
    void add_block(const std::string &message) {
        blocklist.insert(message);
        mode = true;
    }
    void remove_block(const std::string &message) {
        blocklist.erase(message);
        mode = true;
    }
    void add_white(const std::string &message) {
        whitelist.insert(message);
        mode = false;
    }
    void remove_white(const std::string &message) {
        whitelist.erase(message);
        mode = false;
    }
    void clear() {
        blocklist.clear();
        whitelist.clear();
        mode = true;
    }
    Json::Value to_json() const {
        Json::Value J;
        J["block"] = parse_set_to_json(blocklist);
        J["white"] = parse_set_to_json(whitelist);
        J["mode"] = mode;
        return J;
    }
};

class shinxbot : public bot {
private:
    // ===== Runtime flags/state =====
    std::atomic<bool> bot_enabled{true};

    // ===== Logging =====
    std::ofstream LOG_output[3];
    std::mutex log_lock;
    tm last_getlog{};

    // ===== Dynamic module handles (pointer, handler, alias) =====
    // Snapshot-iterated; see module_registry. Load/unload/reload of modules
    // (and the timer stop/start around it) is serialized by
    // module_admin_mutex_, which is never held while dispatching.
    module_registry<processable> functions;
    module_registry<eventprocess> events;
    std::mutex module_admin_mutex_;
    std::set<userid_t> op_list;

    // ===== Core services =====
    heartBeat *recorder = nullptr;
    Timer *mytimer = nullptr;
    archivist *archive = nullptr;

    // ===== module_load desired state (guarded by module_filter_mutex_) =====
    std::set<std::string> enabled_functions;
    std::set<std::string> enabled_events;
    mutable std::mutex module_filter_mutex_;

    // ===== Group policy state =====
    // group_blocklist is read on every group message and written by the
    // bot.block/white commands: read via is_blocked_in_group(), write under
    // an exclusive blocklist_mutex_.
    std::map<groupid_t, blockItem> group_blocklist;
    mutable std::shared_mutex blocklist_mutex_;
    std::thread heartbeat_thread;

    // ===== Group policy helpers =====
    void save_blocklist(); // caller holds blocklist_mutex_

    // ===== module_load helpers =====
    void load_module_filter_config();
    void save_module_filter_config() const; // caller holds module_filter_mutex_
    void add_module_to_filter(const std::string &name, bool is_event);
    void remove_module_from_filter(const std::string &name, bool is_event);
    static std::vector<std::string> list_available_module_names(bool is_event);

    // ===== Module lifecycle =====
    enum class load_result { loaded, reloaded, failed };
    // Load ./lib/<dir>/lib<name>.so (replacing it in place if already
    // loaded). Takes module_admin_mutex_.
    template <typename T>
    load_result load_or_reload(module_registry<T> &reg, const std::string &dir,
                               const std::string &name);
    // Unload `name` if loaded; returns whether it was. Takes
    // module_admin_mutex_.
    template <typename T>
    bool unload_module(module_registry<T> &reg, const std::string &name);
    void unload_all_modules();

    // ===== OP command handlers =====
    bool handle_bot_load(const std::string &message, const msg_meta &conf);
    bool handle_bot_unload(const std::string &message, const msg_meta &conf);
    bool handle_bot_reload(const std::string &message, const msg_meta &conf);
    bool meta_func(const std::string &message, const msg_meta &conf);

    // ===== Networking/log bootstrap =====
    int start_server();
    void refresh_log_stream(); // thread-safe (takes log_lock)
    void refresh_log_stream_unlocked();

    // ===== Lifecycle internals =====
    void init();
    void unload_hooks(const std::string &name);
    void init_func(const std::string &name, processable *p);
    void init_func(const std::string &name, eventprocess *p);

public:
    // ===== Construction =====
    shinxbot(int recv_port, int send_port, const std::string &tk);
    explicit shinxbot(const Json::Value &J);

    // ===== Entry points =====
    bool is_op(const userid_t a) const override;
    void input_process(const std::string &input) override;
    void run() override;
    void setlog(LOG type, std::string message) override;
    void cq_send_all_op(const std::string &message) override;

    // Whether module `name` is disabled for group `gid` (bot.block/white).
    bool is_blocked_in_group(groupid_t gid, const std::string &name) const;

    // ===== Teardown =====
    ~shinxbot() override;
};
