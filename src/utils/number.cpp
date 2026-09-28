#include "utils.h"

#include <iomanip>
#include <string>

std::string to_human_string(const int64_t u) {
    std::stringstream ss;
    if (u > 1000000000)
        ss << std::fixed << std::setprecision(2) << u / 1000000000.0 << "B";
    else if (u > 1000000)
        ss << std::fixed << std::setprecision(2) << u / 1000000.0 << "M";
    else if (u > 1000)
        ss << std::fixed << std::setprecision(2) << u / 1000.0 << "k";
    else
        ss << u;
    return ss.str();
}

namespace {

// Shared parser for the four my_string2* overloads. Accumulates in uint64_t
// so an over-long digit run wraps (as the old code did in practice) instead
// of being signed-overflow UB.
template <typename Str>
uint64_t parse_leading_number(const Str &s, bool &negative) {
    using Ch = typename Str::value_type;
    uint64_t ans = 0;
    bool seen_digit = false;
    negative = false;
    for (const Ch &c : s) {
        if (c == Ch('-')) {
            negative = true;
            seen_digit = true;
        } else if (Ch('0') <= c && c <= Ch('9')) {
            ans = ans * 10 + static_cast<uint64_t>(c - Ch('0'));
            seen_digit = true;
        } else if (seen_digit) {
            break;
        }
    }
    return ans;
}

int64_t to_signed(uint64_t magnitude, bool negative) {
    return static_cast<int64_t>(negative ? 0 - magnitude : magnitude);
}

} // namespace

int64_t my_string2int64(const std::wstring &s) {
    bool negative = false;
    const uint64_t ans = parse_leading_number(s, negative);
    return to_signed(ans, negative);
}

uint64_t my_string2uint64(const std::wstring &s) {
    bool negative = false;
    return parse_leading_number(s, negative);
}

int64_t my_string2int64(const std::string &s) {
    bool negative = false;
    const uint64_t ans = parse_leading_number(s, negative);
    return to_signed(ans, negative);
}

uint64_t my_string2uint64(const std::string &s) {
    bool negative = false;
    return parse_leading_number(s, negative);
}
