#ifndef TEST_MBEDTLS_SHA256_H
#define TEST_MBEDTLS_SHA256_H

#include <cstddef>
#include <cstring>

struct mbedtls_sha256_context {};

inline void mbedtls_sha256_init(mbedtls_sha256_context*) {}
inline void mbedtls_sha256_free(mbedtls_sha256_context*) {}
inline int mbedtls_sha256_starts(mbedtls_sha256_context*, int) { return 0; }
inline int mbedtls_sha256_update(
    mbedtls_sha256_context*,
    const unsigned char*,
    std::size_t
) { return 0; }
inline int mbedtls_sha256_finish(mbedtls_sha256_context*, unsigned char output[32]) {
    std::memset(output, 0, 32);
    return 0;
}

#endif
