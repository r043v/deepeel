#include "msx.h"
#include "explore.h"
void dp_msx_regs(FILE *out) {
    fputs("MSX1 (Z80) hardware registry — metadata, not an execution model\n"
        "Memory: four 16 KiB CPU pages, each selecting a primary slot through $A8 (two bits/page).\n"
        "Bridge profile: slot 0 simulated BIOS in page 0; slot 1 cartridge in $4000-$BFFF; slot 2 empty; slot 3 64 KiB RAM.\n"
        "Expanded slots: $FFFF selects four secondary slots in an expanded primary slot, read complemented; absent from this bridge.\n"
        "Cartridge: AB header, INIT/STATEMENT/DEVICE/TEXT entries; plain, ASCII8/16, Konami and Konami SCC. Mapper detection is heuristic; explicit selection is available in the bridge API.\n"
        "Physical ROM keys: byte offsets, including 8 KiB mapper units; never bank << 16. BIOS and RAM are outside the cartridge map.\n"
        "Ports (independent 16-bit IO bus; this profile decodes the low byte):\n"
        "  $98 rw VDP_DATA       TMS9918A VRAM data; read-ahead; 14-bit increment\n"
        "  $99 r  VDP_STATUS     frame IRQ/sprite flags; acknowledge IRQ and reset control latch\n"
        "  $99 w  VDP_CONTROL    two-byte address or R0-R7 write; bit 7 selects register command\n"
        "  $A0 w  AY_ADDRESS     select AY-3-8910 R0-R15\n"
        "  $A1 w  AY_DATA        selected AY register write\n"
        "  $A2 r  AY_DATA        selected AY register read; R14 joystick/cassette inputs\n"
        "  $A8 rw PPI_A          primary slots: bits 1:0 page 0, 3:2 page 1, 5:4 page 2, 7:6 page 3\n"
        "  $A9 r  PPI_B          active-low keyboard row selected by PPI C bits 3:0\n"
        "  $AA rw PPI_C          keyboard row, cassette motor/data, CAPS LED and key click\n"
        "  $AB w  PPI_CONTROL    8255 mode/direction command, or bit-set/reset of PPI C\n"
        "  $FC-$FF rw RAM_MAPPER optional expanded RAM page registers; absent from this bridge\n"
        "Indirect TMS9918A registers:\n"
        "  R0 graphics mode/external video; R1 display, frame IRQ bit 5, modes, sprite zoom/size\n"
        "  R2 name table; R3 colour table; R4 pattern table; R5 sprite attributes; R6 sprite patterns; R7 colours\n"
        "  Status bit 7 frame pending, bit 6 fifth sprite, bit 5 collision, bits 4:0 fifth sprite index\n"
        "Indirect AY-3-8910 registers:\n"
        "  R0-R5 three 12-bit tone periods; R6 noise period; R7 mixer and port A/B directions\n"
        "  R8-R10 volume/envelope selection; R11-R12 envelope period; R13 envelope shape\n"
        "  R14 port A: active-low joystick, cassette input; R15 port B: joystick select bit 6 and output pins\n"
        "Cartridge mapper windows and write decoders:\n"
        "  ASCII8: $6000-$67FF/$6800-$6FFF/$7000-$77FF/$7800-$7FFF select four 8 KiB windows\n"
        "  ASCII16: $6000-$67FF/$7000-$77FF select two 16 KiB windows\n"
        "  Konami: fixed $4000-$5FFF; $6000-$7FFF/$8000-$9FFF/$A000-$BFFF select other 8 KiB windows\n"
        "  Konami SCC: $5000-$57FF/$7000-$77FF/$9000-$97FF/$B000-$B7FF select four 8 KiB windows\n"
        "  SCC enable: low six bank bits at $9000 equal $3F; $9800-$9FFF decodes SCC while enabled\n"
        "Interrupts: TMS9918A frame level INT, enabled by R1.5, acknowledged by status read; IM1 $0038 normally dispatches BIOS hooks H.KEYI $FD9A and H.TIMI $FD9F.\n"
        "Z80 modes IM0/IM2 and EI delay remain CPU state. AY/PPI have no separate interrupt source in this profile. RETURN is a keyboard key, never console Pause/NMI.\n"
        "Limits: no real BIOS, disk/tape/printer/RS232, expanded slots, RAM mapper, SRAM boards or MSX2 devices. BIOS traps, keyboard, PPI mode control and SCC read-back are partial runtime models. Bounded dynamic exploration and static closure use observed slot/bank views; unresolved BIOS/RAM control flow stops static closure.\n",out);
}

const DpSegaSystem *dp_msx_system(void) {
    static const DpSegaPort ports[]={
        {255,0x98,3,"VDP_DATA","VRAM data/read-ahead"},
        {255,0x99,1,"VDP_STATUS","Frame IRQ acknowledge/control latch reset"},
        {255,0x99,2,"VDP_CONTROL","Address or indirect register command"},
        {255,0xa0,2,"AY_ADDRESS","AY register selection"},
        {255,0xa1,2,"AY_DATA","AY register write"},
        {255,0xa2,1,"AY_DATA","AY register/joystick read"},
        {255,0xa8,3,"PPI_A","Primary slot selection"},
        {255,0xa9,1,"PPI_B","Keyboard row"},
        {255,0xaa,3,"PPI_C","Keyboard row and output pins"},
        {255,0xab,2,"PPI_CONTROL","PPI control/bit set-reset"}
    };
    static const DpSegaSystem system={"msx","MSX1",ports,sizeof(ports)/sizeof(*ports),65536,8,0,
        "Primary slots: BIOS/AB cartridge/empty/64 KiB RAM","Plain, ASCII8/16, Konami/SCC",
        "TMS9918A frame INT; RETURN is polled, never NMI","Partial simulated BIOS; no expanded slots/MSX2"};
    return &system;
}
