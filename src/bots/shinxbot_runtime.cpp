#include "dynamic_lib.hpp"
#include "shinxbot.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <thread>

namespace fs = fs;

void shinxbot::init() {
    // Wait for the backend to be reachable AND logged in: a logged-out backend
    // answers get_login_info without a user_id, and botqq=0 would send logs to
    // log/0/ and key per-bot state on qq 0.
    for (int attempt = 0;; ++attempt) {
        std::string why;
        try {
            Json::Value J = string_to_json(cq_get("get_login_info"));
            const Json::Value &uid = J["data"]["user_id"];
            if (uid.isIntegral() && uid.asUInt64() != 0) {
                botqq = uid.asUInt64();
                break;
            }
            why = "backend not logged in yet";
        } catch (const std::exception &e) {
            why = e.what();
        } catch (const std::string &e) { // do_get throws a std::string
            why = e;
        } catch (...) {
            why = "unknown error";
        }
        if (attempt % 30 == 0) { // first failure, then every ~5 min
            set_global_log(LOG::WARNING,
                           "waiting for backend login (get_login_info): " +
                               why);
        }
        std::this_thread::sleep_for(std::chrono::seconds(10));
    }
    std::cout << "botqq:" << botqq << std::endl;

    refresh_log_stream();

    Json::Value J_op = string_to_json(
        readfile(bot_config_path(this, "core/op_list.json"), "[]"));
    parse_json_to_set(J_op, op_list);

    Json::Value J_block = string_to_json(
        readfile(bot_config_path(this, "core/blocklist.json"), "{}"));
    for (const auto &member : J_block.getMemberNames()) {
        groupid_t gid = 0;
        try { // one bad key must not abort init and crash-loop the bot
            gid = std::stoull(member);
        } catch (const std::exception &) {
            set_global_log(LOG::WARNING,
                           "core/blocklist.json: skipping bad group id '" +
                               member + "'");
            continue;
        }
        group_blocklist[gid] = blockItem(J_block[member]);
    }

    Json::Value J_rec = string_to_json(readfile(
        bot_config_path(this, "core/recover.json"), "{\"commands\":[]}"));
    Json::Value Ja_rec = J_rec["commands"];
    Json::ArrayIndex sz = Ja_rec.size();
    std::vector<std::string> rec_list;
    for (Json::ArrayIndex i = 0; i < sz; ++i) {
        rec_list.push_back(Ja_rec[i].asString());
    }

    recorder = new heartBeat(rec_list);
    for (const auto &px : functions) {
        init_func(std::get<2>(px), std::get<0>(px));
    }
    for (const auto &px : events) {
        init_func(std::get<2>(px), std::get<0>(px));
    }
    this->archive->add_path("MAIN", getConfigDir());
    this->archive->set_default_pwd(std::to_string(this->botqq));
}

namespace {

// Must be called from inside a catch block. Renders the in-flight exception
// the way plugin errors have always been reported ("Throw an ...: ...").
// Plugins throw std::string (do_get/do_post failures) and const char* too.
std::string describe_plugin_exception() {
    try {
        throw;
    } catch (const char *e) {
        return std::string("Throw an char*: ") + (e ? e : "(null)");
    } catch (const std::string &e) {
        return "Throw an string: " + e;
    } catch (const std::exception &e) {
        return std::string("Throw an exception: ") + e.what();
    } catch (...) {
        return "Throw an unknown error";
    }
}

// Must be called from inside a catch block; the bare reason, for log lines.
std::string describe_exception() {
    try {
        throw;
    } catch (const char *e) {
        return e ? e : "(null)";
    } catch (const std::string &e) {
        return e;
    } catch (const std::exception &e) {
        return e.what();
    } catch (...) {
        return "unknown error";
    }
}

} // namespace

void shinxbot::input_process(const std::string &input) {
    if (input.empty()) {
        return;
    }
    Json::Value J = string_to_json(input);
    // isMember() throws on non-object JSON (arrays, scalars)
    if (!J.isObject() || !J.isMember("post_type")) {
        return;
    }
    const std::string post_type = J["post_type"].asString();

    if ((post_type == "request" || post_type == "notice") && bot_enabled) {
        for (const auto &evenx : events) {
            eventprocess *even = std::get<0>(evenx);
            const std::string &ename = std::get<2>(evenx);
            try { // a throwing event must not silently drop the whole tick
                if (even->check(this, J)) {
                    even->process(this, J);
                }
            } catch (const std::exception &e) {
                setlog(LOG::ERROR, "event " + ename + " threw: " + e.what());
            } catch (...) {
                setlog(LOG::ERROR,
                       "event " + ename + " threw: " + describe_exception());
            }
        }
    } else if (post_type == "message") {
        if (J.isMember("message_type") && J.isMember("message")) {
            std::string messageStr = messageArr_to_string(J["message"]);
            int64_t message_id = J["message_id"].asInt64();
            std::string message_type = J["message_type"].asString();
            if (message_type == "group" || message_type == "private" ||
                message_type == "internal") {
                userid_t user_id = 0;
                groupid_t group_id = 0;
                if (J.isMember("group_id")) {
                    group_id = J["group_id"].asUInt64();
                }
                if (J.isMember("user_id")) {
                    user_id = J["user_id"].asUInt64();
                }
                const bool is_group = message_type == "group";
                msg_meta conf = (msg_meta){std::move(message_type), user_id,
                                           group_id, message_id, this};
                bool run_functions = false;
                try { // operator commands were previously unguarded
                    run_functions = meta_func(messageStr, conf);
                } catch (const std::exception &e) {
                    setlog(LOG::ERROR,
                           std::string("meta_func threw: ") + e.what());
                } catch (...) {
                    setlog(LOG::ERROR,
                           "meta_func threw: " + describe_exception());
                }
                if (run_functions && bot_enabled) {
                    // Parsed lazily: only messageArr-aware plugins need it.
                    std::optional<Json::Value> messageArr;
                    for (const auto &funcx : functions) {
                        processable *func = std::get<0>(funcx);
                        const std::string &name = std::get<2>(funcx);
                        if (is_group && is_blocked_in_group(group_id, name)) {
                            continue;
                        }
                        try {
                            if (func->is_support_messageArr()) {
                                if (!messageArr) {
                                    messageArr =
                                        expand_string_to_messageArr(messageStr);
                                }
                                if (func->check(*messageArr, conf)) {
                                    func->process(*messageArr, conf);
                                }
                            } else {
                                if (func->check(messageStr, conf)) {
                                    func->process(messageStr, conf);
                                }
                            }
                        } catch (...) {
                            const std::string what = describe_plugin_exception();
                            setlog(LOG::ERROR, fmt::format("{}: {}", name, what));
                            // The error reply goes through the backend too; if
                            // that throws, keep dispatching to other plugins.
                            try {
                                cq_send(what, conf);
                            } catch (...) {
                                setlog(LOG::ERROR,
                                       name + ": failed to report error: " +
                                           describe_exception());
                            }
                        }
                    }
                }
            }
        }
    } else if (post_type == "meta_event") {
        if (J.isMember("meta_event_type") &&
            J["meta_event_type"].asString() == "heartbeat" &&
            recorder != nullptr) {
            recorder->inform();
        }
    }
}

void shinxbot::run() {
    this->mytimer =
        new Timer(std::chrono::milliseconds(500), this); // smallest time: 1s
    this->archive = new archivist();

    load_module_filter_config();

    auto extract_module_name = [](const std::string &filename) -> std::string {
        if (filename.size() <= 3 ||
            filename.compare(filename.size() - 3, 3, ".so") != 0) {
            return "";
        }
        const size_t start = filename.rfind("lib", 0) == 0 ? 3 : 0;
        return filename.substr(start, filename.size() - 3 - start);
    };

    auto load_all = [&](const fs::path &dir,
                        const std::set<std::string> &enabled,
                        const std::string &kind, auto loader_and_store) {
        try {
            for (const auto &entry : fs::directory_iterator(dir)) {
                if (!(entry.is_regular_file() || entry.is_symlink())) {
                    continue;
                }

                std::string filename = entry.path().filename().string();
                std::string name = extract_module_name(filename);
                if (name.empty()) {
                    continue;
                }

                if (!enabled.empty() && enabled.find(name) == enabled.end()) {
                    set_global_log(LOG::INFO,
                                   fmt::format("Skipped {} by module_load "
                                               "config: {}",
                                               kind, name));
                    continue;
                }

                if (!loader_and_store(entry.path(), name)) {
                    set_global_log(LOG::ERROR,
                                   fmt::format("Error while loading {}:{}",
                                               kind, filename));
                }
            }
        } catch (const fs::filesystem_error &ex) {
            set_global_log(LOG::ERROR,
                           std::string("Error accessing directory: ") +
                               ex.what());
        }
    };

    load_all("./lib/functions/", enabled_functions, "function",
             [&](const fs::path &path, const std::string &name) {
                 auto result = load_function<processable>(path);
                 if (result.first == nullptr) {
                     return false;
                 }
                 functions.push_back(
                     std::make_tuple(result.first, result.second, name));
                 set_global_log(LOG::INFO, "Loaded function: " + name);
                 return true;
             });

    load_all("./lib/events/", enabled_events, "event",
             [&](const fs::path &path, const std::string &name) {
                 auto result = load_function<eventprocess>(path);
                 if (result.first == nullptr) {
                     return false;
                 }
                 events.push_back(
                     std::make_tuple(result.first, result.second, name));
                 setlog(LOG::INFO, "Loaded event: " + name);
                 return true;
             });

    this->init();

    // Greet ops on start, unless suppressed (avoids spamming every op on each
    // crash-recovery re-fork, and lets a deploy start quietly). Set
    // SHINX_QUIET_START=1 in the environment to mute.
    // Read on this thread before the server's worker threads exist.
    if (std::getenv("SHINX_QUIET_START") == nullptr) { // NOLINT(concurrency-mt-unsafe)
        // A greeting that fails to send (backend hiccup right after login)
        // must not throw out of run() and abort the freshly started child.
        try {
            cq_send_all_op("Love you!");
        } catch (...) {
            setlog(LOG::WARNING,
                   "start-up greeting to ops failed: " + describe_exception());
        }
    }
    heartbeat_thread = std::thread(&heartBeat::run, recorder);

    this->mytimer->timer_start();
    this->start_server();
    this->mytimer->timer_stop();

    // The server has stopped (or never bound): stop the heartbeat thread now,
    // it wakes immediately instead of finishing its 60s sleep.
    recorder->stop();
    if (heartbeat_thread.joinable()) {
        heartbeat_thread.join();
    }

    // The server (and its worker threads) is gone, so nothing is dispatching:
    // close every plugin through its own destroy_t.
    unload_all_modules();
}
