#pragma once

#define ESP_GOTO_ON_FALSE(condition, error, label, tag, format, ...) \
    do {                                                               \
        if (!(condition)) {                                            \
            ret = (error);                                             \
            goto label;                                                \
        }                                                              \
    } while (0)

#define ESP_RETURN_ON_FALSE(condition, error, tag, format, ...) \
    do {                                                        \
        if (!(condition)) {                                     \
            return (error);                                     \
        }                                                       \
    } while (0)

#define ESP_GOTO_ON_ERROR(expression, label, tag, format, ...) \
    do {                                                        \
        ret = (expression);                                     \
        if (ret != ESP_OK) {                                    \
            goto label;                                         \
        }                                                       \
    } while (0)
