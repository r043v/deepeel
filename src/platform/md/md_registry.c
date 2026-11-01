#include "md_registry.h"

/* Order matters only for find(): first match wins, narrower ranges first. */
const DpMdReg dp_md_regs[] = {
    {0xC00000, 0xC00003, "VDP_DATA", "video"},
    {0xC00004, 0xC00007, "VDP_CTRL", "video"},
    {0xC00008, 0xC0000F, "VDP_HV", "video"},
    {0xC00010, 0xC00017, "PSG", "sound"},
    {0xC00018, 0xC0001F, "VDP_DEBUG", "video"},
    {0xA00000, 0xA03FFF, "Z80_RAM", "sound"},
    {0xA04000, 0xA04003, "YM2612", "sound"},
    {0xA06000, 0xA060FF, "Z80_BANK", "sound"},
    {0xA10000, 0xA10001, "VERSION", "sys"},
    {0xA10002, 0xA10003, "PAD1_DATA", "input"},
    {0xA10004, 0xA10005, "PAD2_DATA", "input"},
    {0xA10006, 0xA10007, "EXT_DATA", "input"},
    {0xA10008, 0xA10009, "PAD1_CTRL", "input"},
    {0xA1000A, 0xA1000B, "PAD2_CTRL", "input"},
    {0xA1000C, 0xA1000D, "EXT_CTRL", "input"},
    {0xA1000E, 0xA1001F, "SERIAL", "input"},
    {0xA11100, 0xA11101, "Z80_BUSREQ", "sound"},
    {0xA11200, 0xA11201, "Z80_RESET", "sound"},
    {0xA130F0, 0xA130F1, "SRAM_CTRL", "mapper"},
    {0xA130F2, 0xA130FF, "SSF2_BANK", "mapper"},
    {0xA14000, 0xA14003, "TMSS", "sys"},
    {0x200000, 0x20FFFF, "SRAM", "memory"},
    {0x000000, 0x3FFFFF, "ROM", "memory"},
    {0xFF0000, 0xFFFFFF, "WORK_RAM", "memory"},
};
const int dp_md_reg_count = (int)(sizeof dp_md_regs / sizeof dp_md_regs[0]);

const DpMdVector dp_md_vectors[] = {
    {0, "SSP"}, {1, "RESET"}, {2, "BUS_ERROR"}, {3, "ADDRESS_ERROR"},
    {4, "ILLEGAL"}, {5, "DIV_ZERO"}, {6, "CHK"}, {7, "TRAPV"},
    {8, "PRIVILEGE"}, {9, "TRACE"}, {10, "LINE_A"}, {11, "LINE_F"},
    {26, "IRQ2_EXT"}, {28, "IRQ4_HINT"}, {30, "IRQ6_VINT"},
};
const int dp_md_vector_count = (int)(sizeof dp_md_vectors / sizeof dp_md_vectors[0]);

int dp_md_reg_find(uint32_t a) {
    a &= 0xFFFFFFu;
    if (a >= 0xE00000) a |= 0xFF0000; /* work RAM mirrors */
    for (int i = 0; i < dp_md_reg_count; i++)
        if (a >= dp_md_regs[i].lo && a <= dp_md_regs[i].hi) return i;
    return -1;
}
