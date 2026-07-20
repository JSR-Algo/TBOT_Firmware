#ifndef TEST_MBEDTLS_BASE64_H
#define TEST_MBEDTLS_BASE64_H

#include <cstddef>

inline int mbedtls_base64_encode(
    unsigned char*,
    std::size_t,
    std::size_t* output_length,
    const unsigned char*,
    std::size_t input_length
) {
    *output_length = input_length;
    return 0;
}

#endif
