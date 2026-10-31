// cart.h: cartridge image loading (header parse, MBC detection).
#ifndef DP_CART_H
#define DP_CART_H

#include "../common.h"

typedef enum {
    DP_MBC_NONE = 0,
    DP_MBC1,
    DP_MBC2,
    DP_MBC3,
    DP_MBC5,
    DP_MBC_UNKNOWN,
} DpMbc;

typedef struct DpCart {
    uint8_t *rom;
    uint32_t rom_size;
    int rom_banks;        // 16 KiB banks
    uint8_t *ram;         // cartridge RAM image (zeroed)
    uint32_t ram_size;
    DpMbc mbc;
    int cgb;              // 0x143 flag
    char title[17];
} DpCart;

const char *dp_mbc_name(DpMbc mbc);
// Loads a .gb/.gbc file; returns NULL on error (message in err[256] if given).
DpCart *dp_cart_load(const char *path, char *err, size_t errsz);
// Takes ownership of an in-memory rom image (malloc'd or NULL -> zero rom).
DpCart *dp_cart_from_memory(uint8_t *rom, uint32_t size, char *err, size_t errsz);
void dp_cart_free(DpCart *c);

#endif
