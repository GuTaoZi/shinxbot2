#include "utils.h"

#include <cctype>
#include <random>
using u32 = uint_least32_t;
using engine = std::mt19937;

static std::random_device os_seed;
static const u32 seed = os_seed();
static engine generator = engine(seed);

engine get_engine(){
    return generator;
}

int get_random(int maxi)
{
    std::uniform_int_distribution<u32> uni_dis =
        std::uniform_int_distribution<u32>(0, maxi-1);
    return uni_dis(generator);
}
