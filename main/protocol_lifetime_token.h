#ifndef PROTOCOL_LIFETIME_TOKEN_H
#define PROTOCOL_LIFETIME_TOKEN_H

#include <cstdint>

inline bool ProtocolLifetimeMatches(const void* current,
                                    const void* expected,
                                    uint64_t current_generation,
                                    uint64_t expected_generation) {
    return current == expected && current_generation == expected_generation;
}

#endif  // PROTOCOL_LIFETIME_TOKEN_H
