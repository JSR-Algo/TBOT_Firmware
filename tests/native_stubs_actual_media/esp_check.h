#pragma once
#include <stdio.h>
#define ESP_GOTO_ON_FALSE(condition, error, label, tag, format, ...) \
    do { if (!(condition)) { fprintf(stderr, "%s: " format "\n", tag, ##__VA_ARGS__); \
        ret = (error); goto label; } } while (0)
#define ESP_RETURN_ON_FALSE(condition, error, tag, format, ...) \
    do { if (!(condition)) { fprintf(stderr, "%s: " format "\n", tag, ##__VA_ARGS__); \
        return (error); } } while (0)
#define ESP_GOTO_ON_ERROR(expression, label, tag, format, ...) \
    do { ret = (expression); if (ret != ESP_OK) { \
        fprintf(stderr, "%s: " format "\n", tag, ##__VA_ARGS__); goto label; } } while (0)
