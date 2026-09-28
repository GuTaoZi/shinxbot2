#include "dynamic_lib.hpp"
#include "shinxbot.hpp"

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <sstream>

namespace fs = fs;

// Copy a plugin .so to a fresh unique path before dlopen so hot-reload actually
// picks up rebuilt code. glibc refuses to unload C++ .so files containing
// STB_GNU_UNIQUE symbols (std::regex/shared_ptr instantiations) or in-use TLS,
// so dlclose is a no-op and re-dlopen of the SAME path returns the stale code.
// Loading a NEW path each time sidesteps that entirely: the old copy stays
// mapped (harmless) while the fresh code loads. Old copies of the same module
// are unlinked first (safe on Linux — a still-mapped file persists until exit).
// Returns the copy path, or the original path on any failure.
static std::string hot_reload_copy(const std::string &real_so) {
    static std::atomic<unsigned long long> ctr{0};
    try {
        fs::path src(real_so);
        if (!fs::exists(src)) {
            return real_so;
        }
        fs::path dir = src.parent_path() / ".hot";
        fs::create_directories(dir);
        const std::string prefix = src.stem().string() + ".";
        for (const auto &e : fs::directory_iterator(dir)) {
            if (e.path().filename().string().rfind(prefix, 0) == 0) {
                std::error_code ec;
                fs::remove(e.path(), ec);
            }
        }
        fs::path dst = dir / (prefix + std::to_string(ctr++) + ".so");
        fs::copy_file(src, dst, fs::copy_options::overwrite_existing);
        return dst.string();
    } catch (const std::exception &e) {
        set_global_log(LOG::WARNING, "hot-reload copy of " + real_so +
                                         " failed, loading in place: " +
                                         e.what());
        return real_so;
    }
}

// Detach the framework hooks a module registered (timer callbacks, backup
// paths). Must run before the module is destroyed.
void shinxbot::unload_hooks(const std::string &name) {
    if (this->mytimer != nullptr) {
        this->mytimer->remove_callback(name);
    }
    if (this->archive != nullptr) {
        this->archive->remove_path(name);
    }
}

void shinxbot::init_func(const std::string &name, processable *p) {
    p->set_callback([this, name](std::function<void(bot * p)> func) {
        this->mytimer->add_callback(name, std::move(func));
    });
    p->set_backup_files(this->archive, name);
}

void shinxbot::init_func(const std::string &name, eventprocess *p) {}

template <typename T>
shinxbot::load_result shinxbot::load_or_reload(module_registry<T> &reg,
                                               const std::string &dir,
                                               const std::string &name) {
    std::lock_guard<std::mutex> admin(module_admin_mutex_);
    // Take the old instance out first so no new dispatch pass picks it up.
    auto old = reg.take(name);
    const bool was_loaded = old.first != nullptr;
    const bool restart_timer =
        was_loaded && this->mytimer != nullptr && this->mytimer->is_running();
    if (restart_timer) {
        this->mytimer->timer_stop();
    }
    if (was_loaded) {
        unload_hooks(name);
        old.first->close_on_release = true;
        old.first.reset(); // closes now, or when the last dispatch pass ends
    }

    auto u = load_function<T>(hot_reload_copy(dir + "lib" + name + ".so"));
    if (u.first != nullptr) {
        reg.insert(old.second, u.first, u.second, name); // same slot as before
        init_func(name, u.first);
    }
    if (restart_timer) {
        this->mytimer->timer_start();
    }
    if (u.first == nullptr) {
        return load_result::failed;
    }
    return was_loaded ? load_result::reloaded : load_result::loaded;
}

template <typename T>
bool shinxbot::unload_module(module_registry<T> &reg, const std::string &name) {
    std::lock_guard<std::mutex> admin(module_admin_mutex_);
    auto old = reg.take(name);
    if (old.first == nullptr) {
        return false;
    }
    const bool restart_timer =
        this->mytimer != nullptr && this->mytimer->is_running();
    if (restart_timer) {
        this->mytimer->timer_stop();
    }
    unload_hooks(name);
    old.first->close_on_release = true;
    old.first.reset(); // closes now, or when the last dispatch pass ends
    if (restart_timer) {
        this->mytimer->timer_start();
    }
    return true;
}

template shinxbot::load_result
shinxbot::load_or_reload<processable>(module_registry<processable> &,
                                      const std::string &, const std::string &);
template shinxbot::load_result
shinxbot::load_or_reload<eventprocess>(module_registry<eventprocess> &,
                                       const std::string &,
                                       const std::string &);
template bool
shinxbot::unload_module<processable>(module_registry<processable> &,
                                     const std::string &);
template bool
shinxbot::unload_module<eventprocess>(module_registry<eventprocess> &,
                                      const std::string &);

void shinxbot::unload_all_modules() {
    functions.close_all();
    events.close_all();
}

void shinxbot::load_module_filter_config() {
    std::lock_guard<std::mutex> lock(module_filter_mutex_);
    const std::string cfg_path = bot_config_path(this, "core/module_load.json");
    const bool cfg_exists = fs::exists(cfg_path);
    Json::Value J = string_to_json(readfile(cfg_path, "{}"));
    if (!J.isObject()) {
        if (cfg_exists) {
            // Unparseable/non-object file: keep a copy before it gets
            // regenerated below, so a hand-edit typo can't silently wipe the
            // operator's module selection.
            std::error_code ec;
            fs::copy_file(cfg_path, cfg_path + ".corrupt",
                          fs::copy_options::overwrite_existing, ec);
            set_global_log(LOG::ERROR, "module_load.json is not a JSON object; "
                                       "saved a copy to " +
                                           cfg_path + ".corrupt");
        }
        J = Json::Value(Json::objectValue);
    }

    enabled_functions.clear();
    enabled_events.clear();

    const bool has_functions =
        J.isMember("functions") && J["functions"].isArray();
    const bool has_events = J.isMember("events") && J["events"].isArray();
    bool changed = false;

    if (has_functions) {
        parse_json_to_set(J["functions"], enabled_functions);
    } else {
        auto fn_names = list_available_module_names(false);
        enabled_functions =
            std::set<std::string>(fn_names.begin(), fn_names.end());
        changed = true;
    }

    if (has_events) {
        parse_json_to_set(J["events"], enabled_events);
    } else {
        auto ev_names = list_available_module_names(true);
        enabled_events =
            std::set<std::string>(ev_names.begin(), ev_names.end());
        changed = true;
    }

    if (!cfg_exists || changed) {
        save_module_filter_config();
        set_global_log(LOG::INFO,
                       "Initialized/updated module_load.json: functions=" +
                           std::to_string(enabled_functions.size()) +
                           ", events=" + std::to_string(enabled_events.size()));
    }
}

void shinxbot::save_module_filter_config() const {
    Json::Value J(Json::objectValue);
    J["functions"] = parse_set_to_json(enabled_functions);
    J["events"] = parse_set_to_json(enabled_events);
    writefile(bot_config_path(this, "core/module_load.json"),
              Json::FastWriter().write(J));
}

void shinxbot::add_module_to_filter(const std::string &name, bool is_event) {
    std::lock_guard<std::mutex> lock(module_filter_mutex_);
    if (is_event) {
        enabled_events.insert(name);
    } else {
        enabled_functions.insert(name);
    }
    save_module_filter_config();
}

void shinxbot::remove_module_from_filter(const std::string &name,
                                         bool is_event) {
    std::lock_guard<std::mutex> lock(module_filter_mutex_);
    if (is_event) {
        enabled_events.erase(name);
    } else {
        enabled_functions.erase(name);
    }
    save_module_filter_config();
}

std::vector<std::string>
shinxbot::list_available_module_names(bool is_event) {
    std::vector<std::string> names;
    const fs::path dir =
        is_event ? fs::path("./lib/events/") : fs::path("./lib/functions/");
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end;
         it.increment(ec)) {
        const auto &entry = *it;
        std::error_code tec;
        if (!(entry.is_regular_file(tec) || entry.is_symlink(tec))) {
            continue;
        }

        std::string name = entry.path().filename().string();
        if (name.size() <= 3 || name.compare(name.size() - 3, 3, ".so") != 0) {
            continue;
        }
        if (name.rfind("lib", 0) == 0) {
            name.erase(0, 3);
        }
        name.erase(name.length() - 3);
        names.push_back(std::move(name));
    }
    if (ec && ec != std::errc::no_such_file_or_directory) {
        set_global_log(LOG::WARNING, "listing " + dir.string() +
                                         " failed: " + ec.message());
    }

    std::sort(names.begin(), names.end());
    names.erase(std::unique(names.begin(), names.end()), names.end());
    return names;
}

namespace {
// "bot.load function a b" -> returns "function", names={a,b}; `skip` is the
// command prefix length (the dispatcher guarantees the prefix matched).
std::string parse_type_and_names(const std::string &message, size_t skip,
                                 std::vector<std::string> &names) {
    std::istringstream iss(message.substr(std::min(skip, message.size())));
    std::string type;
    iss >> type;
    std::string name;
    while (iss >> name) {
        names.push_back(name);
    }
    return type;
}
} // namespace

bool shinxbot::handle_bot_load(const std::string &message,
                               const msg_meta &conf) {
    std::vector<std::string> names;
    const std::string type = parse_type_and_names(message, 8, names);

    if (names.empty() || (type != "function" && type != "event")) {
        cq_send("useage: bot.load [function|event] name", conf);
        return false;
    }
    const bool is_event = type == "event";

    std::ostringstream oss;
    const auto available = list_available_module_names(is_event);
    for (const auto &n : names) {
        if (std::find(available.begin(), available.end(), n) ==
            available.end()) {
            oss << "load " << n
                << " failed: module not found (use bot.list_alias)" << '\n';
            continue;
        }
        const load_result r =
            is_event ? load_or_reload(events, "./lib/events/", n)
                     : load_or_reload(functions, "./lib/functions/", n);
        if (r == load_result::failed) {
            oss << "load " << n << " failed" << '\n';
            continue;
        }
        add_module_to_filter(n, is_event);
        oss << (r == load_result::reloaded ? "reload " : "load ") << n
            << '\n';
    }
    cq_send(trim(oss.str()), conf);
    return false;
}

bool shinxbot::handle_bot_unload(const std::string &message,
                                 const msg_meta &conf) {
    std::vector<std::string> names;
    const std::string type = parse_type_and_names(message, 10, names);

    if (names.empty() || (type != "function" && type != "event")) {
        cq_send("useage: bot.unload [function|event] name", conf);
        return false;
    }
    const bool is_event = type == "event";

    std::ostringstream oss;
    for (const auto &n : names) {
        const bool found = is_event ? unload_module(events, n)
                                    : unload_module(functions, n);
        if (found) {
            oss << "unload " << n << '\n';
        }
        remove_module_from_filter(n, is_event);
        if (!found) {
            oss << n << " not found (removed from module_load if existed)"
                << '\n';
        }
    }
    cq_send(trim(oss.str()), conf);
    return false;
}

bool shinxbot::handle_bot_reload(const std::string &message,
                                 const msg_meta &conf) {
    std::vector<std::string> names;
    const std::string type = parse_type_and_names(message, 10, names);

    if (type != "function") {
        cq_send("useage: bot.reload function [name|all]", conf);
        return false;
    }

    // One snapshot for the whole command: the modules stay loaded throughout.
    const auto snap = functions.snapshot();
    if (snap->empty()) {
        cq_send("No loaded functions.", conf);
        return false;
    }

    std::ostringstream oss;
    auto reload_one = [&](processable *func, const std::string &alias) {
        bool ok = false;
        try {
            ok = func->reload(conf);
        } catch (const std::exception &e) {
            set_global_log(LOG::ERROR,
                           "reload " + alias + " threw: " + e.what());
        } catch (...) {
            set_global_log(LOG::ERROR,
                           "reload " + alias + " threw unknown error");
        }

        if (ok) {
            oss << "reload " << alias << " ok" << '\n';
        } else {
            oss << "reload " << alias << " skipped (stateless or unsupported)"
                << '\n';
        }
    };

    if (names.empty() || (names.size() == 1 && names[0] == "all")) {
        for (const auto &u : *snap) {
            reload_one(std::get<0>(u), u.name());
        }
        cq_send(trim(oss.str()), conf);
        return false;
    }

    for (const auto &target : names) {
        auto it =
            std::find_if(snap->begin(), snap->end(),
                         [&](const auto &u) { return u.name() == target; });
        if (it != snap->end()) {
            reload_one(std::get<0>(*it), target);
        } else {
            oss << "reload " << target << " failed: not loaded" << '\n';
        }
    }

    cq_send(trim(oss.str()), conf);
    return false;
}
