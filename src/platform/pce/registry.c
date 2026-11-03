// registry.c: the PC Engine hardware registry — the hardware page ($FF,
// mapped at $0000-$1FFF of whatever window holds it), the interrupt
// vectors, the memory regions.  Register offsets are relative to the
// hardware page; the machine semantics live in m0boy's pce.c.
#include "pce.h"

static const DpIoReg pce_io_regs[] = {
    { 0x0000, 0x0000, "VDC_AR", "video", 3, DP_IMPL_IRQ | DP_IMPL_EVENT },
    { 0x0002, 0x0003, "VDC_DATA", "video", 3, DP_IMPL_DMA_SRC | DP_IMPL_MODELLED },
    { 0x0400, 0x0407, "VCE", "video", 3, 0 },
    { 0x0800, 0x0809, "PSG", "sound", 2, 0 },
    { 0x0C00, 0x0C01, "TIMER", "timer", 3, DP_IMPL_IRQ | DP_IMPL_EVENT },
    { 0x1000, 0x1000, "JOYPAD", "input", 3, DP_IMPL_EVENT },
    { 0x1402, 0x1402, "IRQ_MASK", "interrupt", 3, DP_IMPL_IRQ },
    { 0x1403, 0x1403, "IRQ_STATUS", "interrupt", 3, DP_IMPL_IRQ | DP_IMPL_EVENT },
    { 0x1800, 0x1BFF, "CDROM", "sys", 3, 0 },
    { 0x1C00, 0x1FFF, "EXPANSION", "sys", 3, 0 },
};

static const DpIrq pce_irqs[] = {
    { 0, 0xFFF6, "IRQ2/BRK" },
    { 1, 0xFFF8, "IRQ1 (VDC)" },
    { 2, 0xFFFA, "TIMER" },
    { 3, 0xFFFC, "NMI" },
};

static const DpRegion pce_regions[] = {
    { 0x0000, 0x1FFF, "MPR0 window", DP_SPACE_ROM },
    { 0x2000, 0x3FFF, "MPR1 window (WRAM $F8)", DP_SPACE_WRAM },
    { 0x4000, 0xDFFF, "MPR2-6 windows", DP_SPACE_ROM },
    { 0xE000, 0xFFFF, "MPR7 window (bank 0 at reset)", DP_SPACE_ROM },
};

static const DpEntry pce_entries[] = {
    { 0xFFFE, DP_SEED_RESET, "reset" },
    { 0xFFF8, DP_SEED_IRQ, "irq1" },
    { 0xFFFA, DP_SEED_IRQ, "timer" },
    { 0xFFF6, DP_SEED_IRQ, "irq2" },
};

static const DpPlatform pce_platform = {
    "PC Engine (HuC6280 + HuC6270 VDC + HuC6260 VCE)",
    pce_io_regs, (int)(sizeof pce_io_regs / sizeof pce_io_regs[0]),
    pce_irqs, (int)(sizeof pce_irqs / sizeof pce_irqs[0]),
    pce_regions, (int)(sizeof pce_regions / sizeof pce_regions[0]),
    pce_entries, (int)(sizeof pce_entries / sizeof pce_entries[0]),
};

const DpPlatform *dp_pce_platform(void) { return &pce_platform; }

void dp_pce_regs(FILE *out) {
    fputs("PC Engine (HuC6280: 65C02 + MMU, 7.16/1.79 MHz) hardware registry\n"
        "MMU: eight 8 KiB windows, MPR0-7 (TAM/TMA); a CPU address maps to the\n"
        "  21-bit physical bus page:offset. MPR7 = $00 at reset (vectors in page 0)\n"
        "Physical pages:\n"
        "  $00-$7F r   HuCard ROM (384 KiB split: $00-$3F, $40-$7F)\n"
        "  $F8     rw  8 KiB work RAM ($F9-$FB mirrors); the zero page and stack\n"
        "              live at its start ($2000/$2100 with MPR1 = $F8)\n"
        "  $FF     rw  hardware page:\n"
        "Hardware page offsets:\n"
        "  $0000 rw VDC address/status: bit 5 VD vblank, bits 0-4 the other\n"
        "           sources; the read acknowledges them (IRQ1)\n"
        "  $0002-$0003 rw VDC data (VWR/VRR through register 2/1): the ROM->VRAM\n"
        "           copy channel; ST0/ST1/ST2 #imm write the ports directly\n"
        "  $0400-$0407 rw VCE: control, palette address, 9-bit colour data\n"
        "  $0800-$0809 w  PSG: six wave channels, LFO, noise\n"
        "  $0C00-$0C01 rw timer: reload (7 bits), enable; a tick every 1024 cycles\n"
        "  $1000    rw pad: SEL/CLR write, nibble read (two reads per pad)\n"
        "  $1402    rw IRQ mask (bit 0 IRQ2, 1 IRQ1, 2 timer); the read acks the timer\n"
        "  $1403    rw IRQ status; the write acknowledges the timer\n"
        "Block moves TII/TDD/TIN/TIA/TAI: the CPU's own copy channel (TIA/TIN\n"
        "  to $0002 = ROM->VRAM)\n"
        "Vectors: $FFF6 IRQ2/BRK, $FFF8 IRQ1 (VDC), $FFFA timer, $FFFC NMI, $FFFE reset\n", out);
}
