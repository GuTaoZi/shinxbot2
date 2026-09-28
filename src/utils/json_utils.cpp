#include "utils.h"

#include <iostream>
#include <jsoncpp/json/json.h>

Json::Value string_to_json(const std::string &str) {
    Json::Value root;
    Json::Reader re;
    try {
        bool isok = re.parse(str, root);
        if (!isok) {
            set_global_log(LOG::ERROR, "string to json failed: " +
                                           re.getFormattedErrorMessages() +
                                           "\nstring: " + str);
        }
    } catch (const std::exception &e) {
        set_global_log(LOG::ERROR,
                       "string to json exception: " + std::string(e.what()) +
                           "\nstring: " + str);
        throw "Json parse error";
    }
    return root;
}

// Used on hand-edited config lists (block lists, op lists, ...): a stray
// object or a non-numeric entry used to throw Json::LogicError straight out of
// a plugin constructor. Skip and log bad entries instead.
void parse_json_to_set(const Json::Value &J, std::set<uint64_t> &mp) {
    if (!J.isArray()) {
        if (!J.isNull()) {
            set_global_log(LOG::WARNING,
                           "parse_json_to_set: expected an array, ignored");
        }
        return;
    }
    for (const auto &v : J) {
        try {
            mp.insert(v.asUInt64());
        } catch (const Json::Exception &e) {
            set_global_log(LOG::WARNING,
                           std::string("parse_json_to_set: skipped entry: ") +
                               e.what());
        }
    }
}
void parse_json_to_set(const Json::Value &J, std::set<std::string> &mp) {
    if (!J.isArray()) {
        if (!J.isNull()) {
            set_global_log(LOG::WARNING,
                           "parse_json_to_set: expected an array, ignored");
        }
        return;
    }
    for (const auto &v : J) {
        try {
            mp.insert(v.asString());
        } catch (const Json::Exception &e) {
            set_global_log(LOG::WARNING,
                           std::string("parse_json_to_set: skipped entry: ") +
                               e.what());
        }
    }
}

Json::ArrayIndex json_array_find(const Json::Value &J, const uint64_t &data) {
    Json::ArrayIndex sz = J.size();
    for (Json::ArrayIndex i = 0; i < sz; i++) {
        try {
            if (J[i].asUInt64() == data) {
                return i;
            }
        } catch (const Json::Exception &) { // non-numeric entry
            continue;
        }
    }
    return sz;
}

// message format like:
// xx[CQ:at,qq=123456]xx[CQ:image,file=xxx.jpg]xx
Json::Value expand_string_to_messageArr(std::string s) {
    Json::Value messageArr;
    size_t pos = s.find("[CQ:");
    if (pos == std::string::npos) {
        Json::Value jj;
        jj["type"] = "text";
        jj["data"]["text"] = cq_decode(s);
        messageArr.append(jj);
        return messageArr;
    } else {
        size_t last_pos = 0;
        while (pos != std::string::npos) {
            if (pos > last_pos) {
                Json::Value jj;
                jj["type"] = "text";
                jj["data"]["text"] = cq_decode(s.substr(last_pos, pos - last_pos));
                messageArr.append(jj);
            }
            size_t end_pos = s.find(']', pos);
            if (end_pos == std::string::npos) {
                break;
            }
            std::string cq_code = s.substr(pos + 4, end_pos - pos - 4);
            size_t comma_pos = cq_code.find(',');
            Json::Value jj;
            jj["type"] = cq_code.substr(0, comma_pos);
            while (comma_pos < cq_code.size()) {
                size_t next_comma = cq_code.find(',', comma_pos + 1);
                if (next_comma == std::string::npos) {
                    next_comma = cq_code.size();
                }
                std::string item =
                    cq_code.substr(comma_pos + 1, next_comma - comma_pos - 1);
                size_t equal_pos = item.find('=');
                if (equal_pos != std::string::npos) {
                    std::string key = item.substr(0, equal_pos);
                    std::string value = item.substr(equal_pos + 1);
                    jj["data"][key] = cq_decode(value);
                } else {
                    jj["data"]["0"] = cq_decode(item);
                }
                comma_pos = next_comma;
            }
            messageArr.append(jj);
            last_pos = end_pos + 1;
            pos = s.find("[CQ:", last_pos);
        }
        if (last_pos < s.size()) {
            Json::Value jj;
            jj["type"] = "text";
            jj["data"]["text"] = cq_decode(s.substr(last_pos));
            messageArr.append(jj);
        }
        return messageArr;
    }
}