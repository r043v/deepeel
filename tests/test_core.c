// test_core.c: machine routing, mappers, echo RAM, DMA, state hashes.
#include "../src/core/cart.h"
#include "../src/core/machine.h"
#include "../src/platform/platform.h"
#include "test_util.h"

static uint8_t *cart_with_data(void) {
    uint8_t *rom = calloc(1, 0x800000); // 512 banks: bank selects must not fold
    rom[0x147] = 0x19; // MBC5
    rom[0x148] = 0x08;
    rom[0x149] = 0x03; // 32 KiB cart RAM
    for (int i = 0; i < 0xA0; i++) rom[0x3000 + i] = 0xAA;
    rom[0xB000] = 0x99; // bank 2 offset $3000, read through the $7000 window
    return rom;
}

int main(void) {
    char err[256];
    // --- MBC5 banking ---------------------------------------------------------
    {
        MsCart *c = ms_cart_from_memory(cart_with_data(), 0x800000, err, sizeof err);
        MsMachine m;
        ms_machine_init(&m, c, ms_platform_gb());
        CHECK(m.win_romN == 1);
        ms_write(&m, 0x2000, 0, MS_ACC_IO); // MBC5: bank 0 is legal
        CHECK(m.win_romN == 0);
        ms_write(&m, 0x2000, 3, MS_ACC_IO);
        CHECK(m.win_romN == 3);
        ms_write(&m, 0x3000, 1, MS_ACC_IO); // high bit -> 0x103
        CHECK(m.win_romN == 0x103);
        ms_write(&m, 0x3000, 0, MS_ACC_IO);
        ms_write(&m, 0x2000, 2, MS_ACC_IO);
        // bank 2 offset $3000 read through the $7000 window
        CHECK(ms_read(&m, 0x7000, MS_ACC_DATA) == 0x99);
        ms_write(&m, 0x4000, 1, MS_ACC_IO); // ram bank 1
        CHECK(m.win_ram == 1);
        ms_write(&m, 0x0000, 0x0A, MS_ACC_IO); // RAMG enable
        CHECK(ms_map_sram(&m, 0xA000) == 0x2000);
        ms_write(&m, 0xA000, 0x5A, MS_ACC_DATA);
        CHECK(ms_read(&m, 0xA000, MS_ACC_DATA) == 0x5A);
        ms_write(&m, 0x0000, 0x00, MS_ACC_IO); // RAMG disable
        CHECK(ms_read(&m, 0xA000, MS_ACC_DATA) == 0xFF);
        ms_cart_free(c);
    }
    // --- MBC1 mode handling -----------------------------------------------------
    {
        uint8_t *img = calloc(1, 0x200000); // 128 banks: 0x45 must not fold
        img[0x147] = 0x01;
        img[0x148] = 0x07;
        MsCart *c = ms_cart_from_memory(img, 0x200000, err, sizeof err);
        MsMachine m;
        ms_machine_init(&m, c, ms_platform_gb());
        ms_write(&m, 0x2000, 0x05, MS_ACC_IO);
        CHECK(m.win_romN == 5);
        ms_write(&m, 0x6000, 1, MS_ACC_IO); // mode 1
        ms_write(&m, 0x4000, 2, MS_ACC_IO); // hi bits
        CHECK(m.win_romN == 0x45);          // (2<<5)|5
        CHECK(m.win_rom0 == 2);             // mode 1 maps hi bits low
        ms_write(&m, 0x2000, 0, MS_ACC_IO); // bank 0 -> 1 on MBC1
        CHECK(m.win_romN == 0x41);
        ms_cart_free(c);
    }
    // --- echo RAM + HRAM + VRAM bank -------------------------------------------
    {
        MsCart *c = ms_cart_from_memory(cart_with_data(), 0x800000, err, sizeof err);
        MsMachine m;
        ms_machine_init(&m, c, ms_platform_gb());
        ms_write(&m, 0xC123, 0x42, MS_ACC_DATA);
        CHECK(ms_read(&m, 0xE123, MS_ACC_DATA) == 0x42);
        ms_write(&m, 0xFF80 + 5, 0x77, MS_ACC_STACK);
        CHECK(ms_read(&m, 0xFF85, MS_ACC_STACK) == 0x77);
        ms_write(&m, 0xFF4F, 1, MS_ACC_IO); // VBK
        ms_write(&m, 0x8000, 0xEE, MS_ACC_DATA);
        CHECK(ms_read(&m, 0x8000, MS_ACC_DATA) == 0xEE);
        ms_write(&m, 0xFF4F, 0, MS_ACC_IO);
        CHECK(ms_read(&m, 0x8000, MS_ACC_DATA) == 0x00);
        ms_cart_free(c);
    }
    // --- OAM DMA copy channel ----------------------------------------------------
    {
        MsCart *c = ms_cart_from_memory(cart_with_data(), 0x800000, err, sizeof err);
        MsMachine m;
        ms_machine_init(&m, c, ms_platform_gb());
        ms_write(&m, 0xFF46, 0x30, MS_ACC_IO); // DMA from $3000
        CHECK(m.oam[0] == 0xAA);
        CHECK(m.oam[0x9F] == 0xAA);
        ms_cart_free(c);
    }
    // --- state hashes -------------------------------------------------------------
    {
        MsCart *c = ms_cart_from_memory(cart_with_data(), 0x800000, err, sizeof err);
        MsMachine a, b;
        ms_machine_init(&a, c, ms_platform_gb());
        ms_machine_init(&b, c, ms_platform_gb());
        CHECK(ms_state_hash(&a) == ms_state_hash(&b));
        ms_write(&b, 0xC234, 1, MS_ACC_DATA); // through ms_write: page hash updates
        CHECK(ms_state_hash(&a) != ms_state_hash(&b));
        ms_machine_init(&b, c, ms_platform_gb());
        b.io[0x40] = 0x91; // LCDC
        CHECK(ms_state_hash(&a) != ms_state_hash(&b));
        ms_machine_init(&b, c, ms_platform_gb());
        b.pc = 0x200;
        CHECK(ms_state_hash(&a) != ms_state_hash(&b));
        ms_cart_free(c);
    }
    TEST_DONE("test_core");
}
