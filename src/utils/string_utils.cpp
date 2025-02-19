#include "utils.h"

#include <codecvt>
#include <locale>
#include <regex>

static std::wstring_convert<std::codecvt_utf8<wchar_t>> converter;

std::wstring string_to_wstring(const std::string &u)
{
    return converter.from_bytes(u);
}
std::string wstring_to_string(const std::wstring &u)
{
    return converter.to_bytes(u);
}

static std::string whitespaces = "\t\r\n ";
static std::wstring w_whitespaces = L"\t\r\n ";

std::string trim(const std::string &u)
{
    size_t fir = u.find_first_not_of(whitespaces);
    if (fir == std::string::npos) {
        return "";
    }
    size_t las = u.find_last_not_of(whitespaces);
    return u.substr(fir, las - fir + 1);
}

std::wstring trim(const std::wstring &u)
{
    size_t fir = u.find_first_not_of(w_whitespaces);
    if (fir == std::wstring::npos) {
        return L"";
    }
    size_t las = u.find_last_not_of(w_whitespaces);
    return u.substr(fir, las - fir + 1);
}

std::string my_replace(const std::string &s, const char old, const char ne)
{
    // std::string ans;
    // for (size_t i = 0; i < s.length(); i++) {
    //     if (s[i] == old) {
    //         ans += ne;
    //     }
    //     else {
    //         ans += s[i];
    //     }
    // }
    // return ans;
    std::string u = s;
    std::replace(u.begin(), u.end(), old, ne);
    return u;
}

std::string cq_encode(const std::string &input)
{
    std::regex amp("&");
    std::regex lBracket("\\[");
    std::regex rBracket("\\]");
    std::regex comma(",");

    std::string result = std::regex_replace(input, amp, "&amp;");
    result = std::regex_replace(result, lBracket, "&#91;");
    result = std::regex_replace(result, rBracket, "&#93;");
    result = std::regex_replace(result, comma, "&#44;");

    return result;
}

std::string cq_decode(const std::string &input)
{
    std::regex amp("&amp;");
    std::regex lBracket("&#91;");
    std::regex rBracket("&#93;");
    std::regex comma("&#44;");

    std::string result = std::regex_replace(result, lBracket, "[");
    result = std::regex_replace(result, rBracket, "]");
    result = std::regex_replace(result, comma, ",");
    result = std::regex_replace(input, amp, "&");

    return result;
}

std::wstring cq_encode(const std::wstring &input)
{
    std::wregex amp(L"&");
    std::wregex lBracket(L"\\[");
    std::wregex rBracket(L"\\]");
    std::wregex comma(L",");

    std::wstring result = std::regex_replace(input, amp, L"&amp;");
    result = std::regex_replace(result, lBracket, L"&#91;");
    result = std::regex_replace(result, rBracket, L"&#93;");
    result = std::regex_replace(result, comma, L"&#44;");

    return result;
}

std::wstring cq_decode(const std::wstring &input)
{
    std::wregex amp(L"&amp;");
    std::wregex lBracket(L"&#91;");
    std::wregex rBracket(L"&#93;");
    std::wregex comma(L"&#44;");

    std::wstring result = std::regex_replace(result, lBracket, L"[");
    result = std::regex_replace(result, rBracket, L"]");
    result = std::regex_replace(result, comma, L",");
    result = std::regex_replace(input, amp, L"&");

    return result;
}

std::pair<std::string, std::string> split_http_addr(const std::string addr)
{
    size_t p = addr.find("/");
    while (p != addr.npos && ((p > 0 && addr[p - 1] == '/') ||
                              (p < addr.length() && addr[p + 1] == '/'))) {
        p = addr.find("/", p + 1);
    }
    if (p == addr.npos) {
        return std::make_pair(addr, "");
    }
    else {
        return std::make_pair(addr.substr(0, p), addr.substr(p));
    }
}