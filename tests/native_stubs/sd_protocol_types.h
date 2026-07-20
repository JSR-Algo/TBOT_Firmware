#ifndef TEST_SD_PROTOCOL_TYPES_H
#define TEST_SD_PROTOCOL_TYPES_H

#include <cstdint>

typedef struct {
    int capacity;
    int sector_size;
} sdmmc_csd_t;

typedef struct {
    int mfg_id;
    int oem_id;
    char name[8];
    int revision;
    int serial;
    int date;
} sdmmc_cid_t;

typedef struct {
    sdmmc_cid_t cid;
    sdmmc_csd_t csd;
} sdmmc_card_t;

#endif
