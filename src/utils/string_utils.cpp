#include "utils.h"

#include <algorithm>
#include <codecvt>
#include <locale>
#include <utility>
#include <vector>

namespace {

// std::wstring_convert keeps internal conversion state and is not safe to
// share between threads; plugins call these from many threads at once.
std::wstring_convert<std::codecvt_utf8<wchar_t>> &utf8_converter() {
    thread_local std::wstring_convert<std::codecvt_utf8<wchar_t>> conv;
    return conv;
}

constexpr const char *kWhitespaces = "\t\r\n ";
constexpr const wchar_t *kWWhitespaces = L"\t\r\n ";

// Single-pass CQ escaping. Equivalent to the previous chain of four
// std::regex_replace calls (the escape sequences cannot overlap or be
// produced by one another), without compiling four regexes per call —
// these run on every incoming and outgoing message.
template <typename Str> Str cq_encode_impl(const Str &input) {
    using Ch = typename Str::value_type;
    Str result;
    result.reserve(input.size() + input.size() / 8);
    for (Ch c : input) {
        switch (c) {
        case Ch('&'):
            result += {Ch('&'), Ch('a'), Ch('m'), Ch('p'), Ch(';')};
            break;
        case Ch('['):
            result += {Ch('&'), Ch('#'), Ch('9'), Ch('1'), Ch(';')};
            break;
        case Ch(']'):
            result += {Ch('&'), Ch('#'), Ch('9'), Ch('3'), Ch(';')};
            break;
        case Ch(','):
            result += {Ch('&'), Ch('#'), Ch('4'), Ch('4'), Ch(';')};
            break;
        default:
            result.push_back(c);
        }
    }
    return result;
}

template <typename Str> Str cq_decode_impl(const Str &input) {
    using Ch = typename Str::value_type;
    const Ch amp[] = {Ch('&'), Ch('a'), Ch('m'), Ch('p'), Ch(';')};
    const Ch lb[] = {Ch('&'), Ch('#'), Ch('9'), Ch('1'), Ch(';')};
    const Ch rb[] = {Ch('&'), Ch('#'), Ch('9'), Ch('3'), Ch(';')};
    const Ch cm[] = {Ch('&'), Ch('#'), Ch('4'), Ch('4'), Ch(';')};
    Str result;
    result.reserve(input.size());
    const size_t n = input.size();
    size_t i = 0;
    while (i < n) {
        if (input[i] == Ch('&') && i + 5 <= n) {
            if (input.compare(i, 5, amp, 5) == 0) {
                result.push_back(Ch('&'));
                i += 5;
                continue;
            }
            if (input.compare(i, 5, lb, 5) == 0) {
                result.push_back(Ch('['));
                i += 5;
                continue;
            }
            if (input.compare(i, 5, rb, 5) == 0) {
                result.push_back(Ch(']'));
                i += 5;
                continue;
            }
            if (input.compare(i, 5, cm, 5) == 0) {
                result.push_back(Ch(','));
                i += 5;
                continue;
            }
        }
        result.push_back(input[i]);
        ++i;
    }
    return result;
}

// LCS length with two rolling rows instead of an (m+1)x(n+1) table.
template <typename Str> size_t lcs_length(const Str &s1, const Str &s2) {
    const size_t n = s2.length();
    std::vector<size_t> prev(n + 1, 0), cur(n + 1, 0);
    for (size_t i = 1; i <= s1.length(); i++) {
        for (size_t j = 1; j <= n; j++) {
            cur[j] = (s1[i - 1] == s2[j - 1]) ? prev[j - 1] + 1
                                              : std::max(prev[j], cur[j - 1]);
        }
        std::swap(prev, cur);
    }
    return prev[n];
}

} // namespace

std::wstring string_to_wstring(const std::string &u) {
    return utf8_converter().from_bytes(u);
}
std::string wstring_to_string(const std::wstring &u) {
    return utf8_converter().to_bytes(u);
}

std::string trim(const std::string &u) {
    size_t fir = u.find_first_not_of(kWhitespaces);
    if (fir == std::string::npos) {
        return "";
    }
    size_t las = u.find_last_not_of(kWhitespaces);
    return u.substr(fir, las - fir + 1);
}

bool starts_with(const std::string &s, const std::string &prefix) {
    return s.size() >= prefix.size() &&
           s.compare(0, prefix.size(), prefix) == 0;
}

bool starts_with(const std::wstring &s, const std::wstring &prefix) {
    return s.size() >= prefix.size() &&
           s.compare(0, prefix.size(), prefix) == 0;
}

std::wstring trim(const std::wstring &u) {
    size_t fir = u.find_first_not_of(kWWhitespaces);
    if (fir == std::wstring::npos) {
        return L"";
    }
    size_t las = u.find_last_not_of(kWWhitespaces);
    return u.substr(fir, las - fir + 1);
}

std::string my_replace(const std::string &s, const char old, const char ne) {
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

std::string cq_encode(const std::string &input) {
    return cq_encode_impl(input);
}

std::string cq_decode(const std::string &input) {
    return cq_decode_impl(input);
}

std::wstring cq_encode(const std::wstring &input) {
    return cq_encode_impl(input);
}

std::wstring cq_decode(const std::wstring &input) {
    return cq_decode_impl(input);
}

std::pair<std::string, std::string> split_http_addr(const std::string &addr) {
    size_t p = addr.find('/');
    while (p != addr.npos && ((p > 0 && addr[p - 1] == '/') ||
                              (p < addr.length() && addr[p + 1] == '/'))) {
        p = addr.find('/', p + 1);
    }
    if (p == addr.npos) {
        return std::make_pair(addr, "");
    } else {
        return std::make_pair(addr.substr(0, p), addr.substr(p));
    }
}

float similarity(const std::string &s1, const std::string &s2) {
    return (float)lcs_length(s1, s2) / s2.length();
}

float similarity(const std::wstring &s1, const std::wstring &s2) {
    return (float)lcs_length(s1, s2) / s2.length();
}
