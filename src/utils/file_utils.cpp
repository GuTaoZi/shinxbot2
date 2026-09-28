#include "utils.h"

#include <atomic>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <jsoncpp/json/json.h>
#include <sstream>
#include <stdexcept>
#include <unistd.h>

namespace {

// Wrap `s` in single quotes for /bin/sh; embedded quotes become '\''.
std::string shell_quote(const std::string &s) {
    std::string out;
    out.reserve(s.size() + 2);
    out.push_back('\'');
    for (char c : s) {
        if (c == '\'') {
            out += "'\\''";
        } else {
            out.push_back(c);
        }
    }
    out.push_back('\'');
    return out;
}

void create_parent_dirs(const fs::path &file_path) {
    const fs::path parent = file_path.parent_path();
    if (parent.empty()) {
        return; // bare relative filename: nothing to create
    }
    std::error_code ec;
    fs::create_directories(parent, ec); // failure surfaces at open time
}

// Plain truncate-and-write (or append); the pre-atomic behaviour, still used
// for appends and as a fallback when the atomic replace is not possible.
void write_in_place(const fs::path &file_path, const std::string &content,
                    bool is_append) {
    if (!fs::exists(file_path)) {
        create_parent_dirs(file_path);
    }
    std::fstream ofile(file_path, is_append ? std::ios::app : std::ios::out);
    if (!ofile.is_open()) { // don't silently drop the write (e.g. config saves)
        // the same two lines the openfile()-based version logged
        set_global_log(LOG::ERROR, "Cannot open file: " + file_path.string());
        set_global_log(LOG::ERROR,
                       "writefile: cannot open " + file_path.string());
        return;
    }
    ofile << content;
    ofile.flush();
    ofile.close();
}

} // namespace

std::fstream openfile(const fs::path file_path,
                      const std::ios_base::openmode mode) {
    if (!fs::exists(file_path)) {
        create_parent_dirs(file_path);
    }
    std::fstream file(file_path, mode);
    if (file.is_open()) {
        return file;
    }
    set_global_log(LOG::ERROR, "Cannot open file: " + file_path.string());
    throw std::runtime_error(file_path.string() + ": open file failed");
}

std::string readfile(const fs::path &file_path,
                     const std::string &default_content) {
    std::ifstream afile(file_path, std::ios::in);
    if (afile.is_open()) {
        std::ostringstream oss;
        oss << afile.rdbuf();
        return oss.str();
    }

    {
        set_global_log(LOG::WARNING,
                       "Reading file: " + file_path.string() +
                           " not exist, using default content...");
        try {
            std::fstream ofile = openfile(file_path, std::ios::out);
            ofile << default_content;
            ofile.flush();
            ofile.close();
            return default_content;
        } catch (const std::exception &) {
            return ""; // openfile already logged why
        }
    }
}

void writefile(const fs::path file_path, const std::string &content,
               bool is_append) {
    if (is_append) {
        write_in_place(file_path, content, true);
        return;
    }

    // Overwrite atomically: write a sibling temp file and rename() it over
    // the target. A crash/kill mid-write (the fork-loop restarts the bot) or
    // two threads saving the same state file then can never leave a
    // truncated/interleaved JSON behind — readers see the old or new file.
    std::error_code ec;
    fs::path target = file_path;
    if (fs::is_symlink(target, ec)) {
        fs::path resolved = fs::canonical(target, ec);
        if (ec) { // dangling link: keep the old write-through behaviour
            write_in_place(file_path, content, false);
            return;
        }
        target = std::move(resolved); // replace the link's target, not the link
    }
    create_parent_dirs(target);

    static std::atomic<unsigned long> tmp_seq{0};
    fs::path tmp = target;
    tmp += ".tmp." + std::to_string(::getpid()) + "." +
           std::to_string(tmp_seq.fetch_add(1, std::memory_order_relaxed));

    bool written = false;
    {
        std::ofstream out(tmp, std::ios::out | std::ios::trunc);
        if (out.is_open()) {
            out << content;
            out.flush();
            written = static_cast<bool>(out);
        }
    }
    if (written) {
        const fs::file_status st = fs::status(target, ec);
        if (!ec && fs::exists(st)) { // keep the existing file's mode
            fs::permissions(tmp, st.permissions(), ec);
        }
        fs::rename(tmp, target, ec);
        if (!ec) {
            return;
        }
    }
    fs::remove(tmp, ec);
    // Could not replace atomically (e.g. read-only directory, disk full):
    // fall back to writing in place so the save isn't lost outright.
    write_in_place(file_path, content, false);
}

void command_download(const std::string &httpAddress,
                      const std::string &filePath, const std::string &fileName,
                      const bool proxy) {
    fs::path p(filePath);
    if (!fs::exists(p)) {
        fs::create_directories(p);
    }
    p /= fileName;
    // std::cout<<p.string()<<std::endl;
    // Single-quote both arguments: the URL/filename may come from chat, and
    // inside double quotes the shell would still expand $(), `` and "".
    std::string command =
        fmt::format("curl -o {} {} {} > /dev/null 2>&1", shell_quote(p.string()),
                    proxy ? "" : "--noproxy '*'", shell_quote(httpAddress));

    int ret = system(command.c_str());
    if (ret) {
        std::ostringstream oss;
        oss << "download " << httpAddress << " to " << filePath << "/"
            << fileName << " Proxy=" << proxy << "failed.";
        set_global_log(LOG::ERROR, oss.str());
    }
}

void download(const std::string &httpAddress, const fs::path &filePath,
              const std::string &fileName, const bool proxy) {
    try {
        fs::create_directories(filePath);
        auto ret = split_http_addr(httpAddress);
        std::string data = do_get(ret.first, ret.second, false, {}, proxy);
        std::fstream ofile;
        try {
            ofile =
                openfile(filePath / fileName, std::ios::out | std::ios::binary);
        } catch (...) {
            set_global_log(LOG::ERROR, "Cannot open file " +
                                           (filePath / fileName).string() +
                                           " for download.");
            return;
        }
        ofile << data;
        ofile.flush();
        ofile.close();
    } catch (const std::exception &e) {
        set_global_log(LOG::ERROR, "At download from" + httpAddress + " to " +
                                       (filePath / fileName).string() +
                                       ", Exception occurred: " + e.what());
        throw;
    } catch (const std::string &e) {
        set_global_log(LOG::ERROR, "At download from" + httpAddress + " to " +
                                       (filePath / fileName).string() +
                                       ", Exception occurred: " + e);
        throw;
    }
}