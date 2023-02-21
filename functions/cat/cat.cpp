#include "cat.h"

#include <fstream>
#include <string>
#include <ctime>
#include <filesystem>
#include <cstdlib>
#include <regex>
#include <jsoncpp/json/json.h>

using namespace std;

std::string place_to_string[] = {"living_room", "kitchen", "bedroom", "bathroom"};
string colors[] = {"black", "white", "gray", "orange", "brown"};
string patterns[] = {"solid", "tabby", "tortoiseshell", "calico", "siamese"};

std::string Cat::generateRandomColor()
{
    return colors[rand() % 5];
}

std::string Cat::generateRandomPattern()
{
    return patterns[rand() % 5];
}

std::string Cat::getLocationString(Place location)
{
    if (location >= 4)
    {
        return "";
    }
    else
    {
        return place_to_string[location];
    }
}

std::string Cat::get_random_text(const Json::Value &J)
{
    Json::ArrayIndex sz = J.size();
    std::string res = J[get_random(sz)].asString();
    size_t pos = res.find("{cat_name}");
    while (pos != std::string::npos) {
        res.replace(pos, 10, name);
        pos = res.find("{cat_name}", pos + name.length());
    }

    return res;
}

std::string Cat::getinfo()
{
    Json::Value J;
    J["name"] = name;
    J["color"] = bodyColor;
    J["pattern"] = colorPattern;
    J["food"] = food;
    J["water"] = water;
    J["affection"] = affection;
    J["visit"] = lastVisitTime;
    J["place"] = location;
    return J.toStyledString();
}
std::string Cat::get_humanread_info()
{
    std::ostringstream oss;
    oss << "name:\t" << name <<std::endl;
    oss << "color:\t" << bodyColor <<std::endl;
    oss << "pattern:\t" << colorPattern <<std::endl;
    oss << "food: " << (food > 50 ? "good" : "hungry") <<std::endl;
    oss << "water: " << (water > 50 ? "good" : "thirsty") <<std::endl;
    oss << "affection: " << (affection > 50 ? "good" : "not good") <<std::endl;
    oss << "place: " << getLocationString(location);

    return oss.str();
}

void Cat::save_cat()
{
    ofstream outfile("./config/cats/" + std::to_string(_id) + ".json");
    if (!outfile.is_open())
    {
        std::filesystem::create_directory("./config/cats");
        outfile.open("./config/cats/" + std::to_string(_id) + ".json");
    }
    outfile << getinfo();
    outfile.close();
}

Cat::Cat(const std::string &name, int64_t user_id) : _id(user_id)
{
    this->name = name;
    this->food = 50;
    this->water = 50;
    this->affection = 50;
    this->lastVisitTime = time(nullptr);
    this->location = static_cast<Place>(rand() % 4);
    this->bodyColor = generateRandomColor();
    this->colorPattern = generateRandomPattern();
    save_cat();
}

Cat::Cat(int64_t user_id) : _id(user_id)
{
    std::ifstream afile;
    afile.open("./config/cats/" + std::to_string(_id) + ".json", std::ios::in);

    if (afile.is_open())
    {
        std::string ans, line;
        while (!afile.eof())
        {
            getline(afile, line);
            ans += line + "\n";
        }
        afile.close();

        Json::Value J = string_to_json(ans);

        name = J["name"].asString();
        bodyColor = J["color"].asString();
        colorPattern = J["pattern"].asString();
        food = J["food"].asInt();
        water = J["water"].asInt();
        affection = J["affection"].asInt();
        lastVisitTime = J["visit"].asInt64();
        location = static_cast<Place>(J["place"].asInt64());
    }
    else
    {
        setlog(LOG::ERROR, "Missing cat file, user: " + std::to_string(user_id));
    }
}

std::string Cat::adopt()
{
    return get_random_text(catmain::get_text()["adopt"]) + "\n" + get_humanread_info();
}
std::string Cat::intro()
{
    return get_random_text(catmain::get_text()["intro"]) + "\n" + get_humanread_info();
}
std::string Cat::pat()
{
    this->affection += 10;
    this->affection = std::min(this->affection, 255);
    this->lastVisitTime = time(nullptr);
    save_cat();
    return get_random_text(catmain::get_text()["pat"]);
}
std::string Cat::feed()
{
    if(this->food >= 90){
        this->lastVisitTime = time(nullptr);
        save_cat();
        return get_random_text(catmain::get_text()["feed"]["full"]);
    }
    this->food += 10;
    this->water += 5;
    this->affection += 5;
    this->water = std::min(this->water, 100);
    this->affection = std::min(this->affection, 255);
    this->lastVisitTime = time(nullptr);
    save_cat();
    return get_random_text(catmain::get_text()["feed"]["hungry"]);
}
std::string Cat::water_f()
{
    if(this->water >= 90){
        this->lastVisitTime = time(nullptr);
        save_cat();
        return get_random_text(catmain::get_text()["water"]["full"]);
    }
    this->water += 10;
    this->lastVisitTime = time(nullptr);
    save_cat();
    return get_random_text(catmain::get_text()["water"]["thirsty"]);
}
std::string Cat::play()
{
    this->affection += 10;
    this->food -= 5;
    this->food = std::max(this->food, 0);
    this->affection = std::min(this->affection, 255);
    this->lastVisitTime = time(nullptr);
    save_cat();
    return get_random_text(catmain::get_text()["play"]);
}
std::string Cat::care()
{
    this->food += 5;
    this->water += 5;
    this->affection += 5;
    this->food = std::min(this->food, 100);
    this->water = std::min(this->water, 100);
    this->affection = std::min(this->affection, 255);
    this->lastVisitTime = time(nullptr);
    save_cat();
    return get_random_text(catmain::get_text()["care"]);
}
std::string Cat::move()
{
    Place new_pos;
    do{
        new_pos = static_cast<Place>(get_random(4));
    }while(new_pos == location);
    location = new_pos;
    std::ostringstream oss;
    oss << "Your cat moved to " + getLocationString(location) << std::endl;
    oss << get_random_text(catmain::get_text()["place"][getLocationString(location)]);
    return oss.str();
}

std::string Cat::process(const std::string &input)
{
    if (time(nullptr) - this->lastVisitTime > 3600 * 6)
    {
        this->affection -= 10;
        this->water -= int(10.0 * (time(nullptr) - this->lastVisitTime) / 3600 / 6);
        this->water -= int(10.0 * (time(nullptr) - this->lastVisitTime) / 3600 / 6);
    }
    std::string res;

    if(affection <= 50 && get_random(3) == 0){
        res += get_random_text(catmain::get_text()["affection_low"]) + "\n";
    }

    if (time(nullptr) - this->lastVisitTime > 60)
    {
        res += move() + "\n";
    }
    if (input.find("intro") != std::string::npos)
    {
        res += intro();
    }
    else if (input.find("pat") != std::string::npos)
    {
        res += pat();
    }
    else if (input.find("feed") != std::string::npos)
    {
        res += feed();
    }
    else if (input.find("water") != std::string::npos)
    {
        res += water_f();
    }
    else if (input.find("play") != std::string::npos)
    {
        res += play();
    }
    else if (input.find("care") != std::string::npos)
    {
        res += care();
    }
    else{
        res += "I don't recognize that.";
    }
    return res;
}