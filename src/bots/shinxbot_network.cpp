#include "shinxbot.hpp"

#include <fmt/core.h>
#include <httplib.h>
#include <thread>

int shinxbot::start_server() {
    httplib::Server svr;

    svr.Post("/", [this](const httplib::Request &req, httplib::Response &res) {
        // Always ack the backend: an exception escaping here would turn into
        // an HTTP 500 that NapCat logs as a failed report, with no trace of
        // the cause in our logs.
        try {
            input_process(req.body);
        } catch (const std::exception &e) {
            setlog(LOG::ERROR,
                   std::string("input_process threw: ") + e.what());
        } catch (const std::string &e) {
            setlog(LOG::ERROR, "input_process threw: " + e);
        } catch (const char *e) {
            setlog(LOG::ERROR, std::string("input_process threw: ") + e);
        } catch (...) {
            setlog(LOG::ERROR, "input_process threw unknown error");
        }

        res.set_content("{}", "application/json");
    });

    set_global_log(LOG::INFO, fmt::format("Server is starting on port {}...",
                                          receive_port));
    const bool ok = svr.listen("127.0.0.1", receive_port);
    if (!ok) {
        set_global_log(LOG::ERROR,
                       fmt::format("Server failed to listen on 127.0.0.1:{} "
                                   "(port in use?)",
                                   receive_port));
    }
    return ok;
}
