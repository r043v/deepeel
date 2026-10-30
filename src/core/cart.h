// cart.h: cartridge image loading (header parse, MBC detection).
#ifndef MS_CART_H
#define MS_CART_H

#include "../common.h"

typedef enum {
    MS_MBC_NONE = 0,
    MS_MBC1,
    MS_MBC2,
    MS_MBC3,
    MS_MBC5,
    MS_MBC_UNKNOWN,
} MsMbc;

typedef struct MsCart {
    uint8_t *rom;
    uint32_t rom_size;
    int rom_banks;        // 16 KiB banks
    uint8_t *ram;         // cartridge RAM image (zeroed)
    uint32_t ram_size;
    MsMbc mbc;
    int cgb;              // 0x143 flag
    char title[17];
} MsCart;

const char *ms_mbc_name(MsMbc mbc);
// Loads a .gb/.gbc file; returns NULL on error (message in err[256] if given).
MsCart *ms_cart_load(const char *path, char *err, size_t errsz);
// Takes ownership of an in-memory rom image (malloc'd or NULL -> zero rom).
MsCart *ms_cart_from_memory(uint8_t *rom, uint32_t size, char *err, size_t errsz);
void ms_cart_free(MsCart *c);

#endif
