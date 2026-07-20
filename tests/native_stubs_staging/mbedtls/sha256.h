#ifndef TEST_MBEDTLS_SHA256_H
#define TEST_MBEDTLS_SHA256_H

#include <cstddef>
#include <cstring>

struct mbedtls_sha256_context {
    unsigned char state[32];
    std::size_t offset;
};

inline void mbedtls_sha256_init(mbedtls_sha256_context* context) {
    std::memset(context->state, 0, sizeof(context->state));
    context->offset = 0;
}
inline void mbedtls_sha256_free(mbedtls_sha256_context*) {}
inline int mbedtls_sha256_starts(mbedtls_sha256_context* context, int) {
    std::memset(context->state, 0, sizeof(context->state));
    context->offset = 0;
    return 0;
}
inline int mbedtls_sha256_update(
    mbedtls_sha256_context* context,
    const unsigned char* input,
    std::size_t length
) {
    for (std::size_t index = 0; index < length; ++index) {
        context->state[context->offset % 32] ^= input[index];
        ++context->offset;
    }
    return 0;
}
inline int mbedtls_sha256_finish(
    mbedtls_sha256_context* context,
    unsigned char output[32]
) {
    std::memcpy(output, context->state, 32);
    return 0;
}

#endif
