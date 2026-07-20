#ifndef JSON_PAYLOAD_SAFETY_H
#define JSON_PAYLOAD_SAFETY_H

#include <cstddef>

// Reject decoded NULs while raw JSON bytes still retain their exact length.
bool JsonHasForbiddenDecodedNull(const char* json, std::size_t length);

#endif  // JSON_PAYLOAD_SAFETY_H
