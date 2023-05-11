#include "forwarder.h"
#include "utils.h"

#include <iostream>

inline bool check_valid(const point_t &a, const point_t &b)
{
    return (a.first == -1 || b.first == -1 || a.first == b.first) &&
           (a.second == -1 || b.second == -1 || a.second == b.second);
}

forwarder::forwarder()
{
    Json::Value J = string_to_json(readfile("./config/forwarder.json", "[]"));
    for (Json::Value j : J) {
        point_t from, to;
        from = std::make_pair<int64_t, int64_t>(j["from"]["group_id"].asInt64(),
                                                j["from"]["user_id"].asInt64());
        to = std::make_pair<int64_t, int64_t>(j["to"]["group_id"].asInt64(),
                                              j["to"]["user_id"].asInt64());
        forward_set.insert(std::make_pair(from, to));
    }
}

void forwarder::save()
{
    Json::Value Ja;
    for (auto it : forward_set) {
        Json::Value J;
        J["from"]["group_id"] = it.first.first;
        J["from"]["user_id"] = it.first.second;
        J["to"]["group_id"] = it.second.first;
        J["to"]["user_id"] = it.second.second;
        Ja.append(J);
    }
    writefile("./config/forwarder.json", Ja.toStyledString());
}

size_t forwarder::configure(std::string message)
{
    size_t t;
    if (message.find("forward.set") == 0) {
        std::istringstream iss(message.substr(11));
        point_t from, to;
        iss >> from.first >> from.second >> to.first >> to.second;
        t = forward_set.insert(std::make_pair(from, to)).second;
    } else if(message.find("forward.del") == 0){
        std::istringstream iss(message.substr(11));
        point_t from, to;
        iss >> from.first >> from.second >> to.first >> to.second;
        t = forward_set.erase(std::make_pair(from, to));
    }
    save();
    return t;
}

void forwarder::process(std::string message, const msg_meta &conf)
{
    if (is_op(conf.user_id) && message.find("forward.") == 0) {
        int t = configure(message);
        cq_send("done with code: " + std::to_string(t), conf);
        return;
    }
    point_t fr = std::make_pair(conf.group_id, conf.user_id);
    std::string group_name, user_name, all_msg;
    bool flg = false;
    for(auto it : forward_set){
        if(check_valid(it.first, fr)){
            if(!flg && it.first.first != -1){
                Json::Value qst;
                qst["group_id"] = it.first.first;
                group_name = string_to_json(cq_send("get_group_info", qst))["group_name"].asString();
                flg = true;
            }
            user_name = get_username(it.first.second, conf.group_id) + "(" + std::to_string(it.first.second) + "): ";
            all_msg = it.first.first == -1 ? user_name : group_name + " " + user_name;
            if(it.second.first == -1){
                cq_send(all_msg + message, (msg_meta){"private", it.second.second, it.second.first, 0});
            } else {
                cq_send(all_msg + message, (msg_meta){"group", -1, it.second.first, 0});
            }
        }
    }
}
bool forwarder::check(std::string message, const msg_meta &conf)
{
    return true;
}
std::string forwarder::help() { return ""; }