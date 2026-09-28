#include "endianness.h"
#include "utils.h"
#include "uuid_v4.h"

// One generator per thread: UUIDGenerator wraps a std::mt19937_64 that is not
// safe to share, and plugins generate UUIDs from their own threads. Each one
// is seeded from std::random_device, so threads don't repeat each other.
std::string generate_uuid() {
    thread_local UUIDv4::UUIDGenerator<std::mt19937_64> generator;
    return generator.getUUID().str();
}
