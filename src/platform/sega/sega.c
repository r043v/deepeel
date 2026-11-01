#include "sega.h"
#include <string.h>
#define COUNT(a) (sizeof(a)/sizeof((a)[0]))
// Upper eight port address bits are not decoded by these console ASICs.
static const DpSegaPort sg_ports[] = {
    {0x00c0,0x0040,2,"SN76489","tone/noise/volume latch and data"},
    {0x00c1,0x0080,3,"VDP_DATA","16 KiB VRAM; read-ahead buffer; auto-increment"},
    {0x00c1,0x0081,1,"VDP_STATUS","VBlank/sprite flags; acknowledge IRQ; clear control latch"},
    {0x00c1,0x0081,2,"VDP_CONTROL","two-byte address/register command; eight TMS9918A registers"},
    {0x00c1,0x00c0,1,"JOYPAD_A","active-low player 1 and player 2 inputs"},
    {0x00c1,0x00c1,1,"JOYPAD_B","active-low player 2 inputs"},
};
#define SMS_PORTS \
    {0x00c1,0x0001,2,"IO_CONTROL","TR/TH direction/output; TH edge latches H counter"}, \
    {0x00c1,0x0040,1,"V_COUNTER","vertical counter; discontinuities depend on video mode/region"}, \
    {0x00c1,0x0041,1,"H_COUNTER","latched horizontal counter"}, \
    {0x00c0,0x0040,2,"SN76489","tone/noise/volume latch and data"}, \
    {0x00c1,0x0080,3,"VDP_DATA","VRAM/CRAM selected by command; buffered VRAM reads; increment"}, \
    {0x00c1,0x0081,1,"VDP_STATUS","acknowledge frame/line IRQ; sprite flags; clear control latch"}, \
    {0x00c1,0x0081,2,"VDP_CONTROL","two-byte command; R0-R10; R10 line interrupt reload"}, \
    {0x00c1,0x00c0,1,"JOYPAD_A","active-low input; TR/TH direction/output affects reads"}, \
    {0x00c1,0x00c1,1,"JOYPAD_B","player 2, reset, TH inputs"}
static const DpSegaPort sms_ports[] = {
    // Exact Mark III/FM ports take precedence over broad controller mirrors.
    {0x00ff,0x00f0,2,"YM2413_ADDRESS","optional Japanese FM sound register address"},
    {0x00ff,0x00f1,2,"YM2413_DATA","optional Japanese FM sound register data"},
    {0x00ff,0x00f2,3,"AUDIO_CONTROL","optional Japanese FM/PSG selection and detection"},
    {0x00c1,0x0000,2,"MEMORY_CONTROL","BIOS/cartridge/card/expansion/work-RAM/IO disable gates"},
    SMS_PORTS,
};
static const DpSegaPort gg_ports[] = {
    {0x00ff,0x0000,1,"GG_START","Start, region and video-standard inputs"},
    {0x00ff,0x0001,3,"GG_PARALLEL_DATA","EXT parallel data"},
    {0x00ff,0x0002,3,"GG_PARALLEL_DIRECTION","EXT direction and NMI enable"},
    {0x00ff,0x0003,3,"GG_SERIAL_TX","serial transmit byte"},
    {0x00ff,0x0004,1,"GG_SERIAL_RX","serial receive byte"},
    {0x00ff,0x0005,3,"GG_SERIAL_CONTROL","serial clock/start/status; interrupt control"},
    {0x00ff,0x0006,2,"GG_STEREO","SN76489 left/right channel masks"},
    {0x00ff,0x003e,2,"MEMORY_CONTROL","work-RAM disable; absent boot BIOS; cartridge and IO gates ignored"},
    SMS_PORTS,
};
static const DpSegaSystem systems[] = {
    {"sg", "SG-1000", sg_ports, COUNT(sg_ports), 1024, 8, 0,
     "$0000-$BFFF cartridge ROM; $C000-$FFFF 1 KiB RAM mirrored every $400",
     "Unbanked baseline; cartridge RAM and third-party banking require a board profile",
     "Reset $0000; RST $00..$38; TMS9918A VBlank level IRQ (R1.5 enable, status read acknowledge); Pause rising-edge NMI $0066. IM0 uses external bus opcode; IM1 $0038; IM2 vector table (I:bus byte).",
     "Baseline SG-1000 only; SC-3000 keyboard/PPI, SG-II and cartridge extensions are not described"},
    {"sms", "Master System", sms_ports, COUNT(sms_ports), 8192, 11, 32,
     "$0000-$BFFF three 16 KiB ROM slots; $C000-$DFFF RAM, mirror $E000-$FFFF; mapper writes also hit underlying RAM",
     "Sega: $FFFC RAM/control (RAM enable bit 3, bank bit 2), $FFFD/$FFFE/$FFFF slots 0/1/2; first $400 bytes fixed bank 0. Runtime autodetects Codemasters/Korean150; CLI --mapper selects auto/sega/codemasters/korean150. Other boards require new profiles.",
     "Reset $0000; VDP frame IRQ R1.5 and line IRQ R0.4 share INT (IM1 $0038), acknowledged by status read; line reload R10. Pause edge NMI $0066; EI deferred one instruction; IM0/IM2 are CPU modes, not additional fixed vectors.",
     "BIOS/card/expansion, region/revision, light-gun/paddle and FM presence need machine profiles; embedded runtime does not implement every registered device"},
    {"gg", "Game Gear", gg_ports, COUNT(gg_ports), 8192, 11, 64,
     "$0000-$BFFF three 16 KiB ROM slots; $C000-$DFFF RAM mirrored at $E000; 160x144 LCD crop of VDP raster",
     "Sega mapper $FFFC-$FFFF; fixed first $400 bytes; optional cartridge RAM; runtime autodetects Codemasters/Korean150; CLI --mapper selects auto/sega/codemasters/korean150; other boards require new profiles",
     "VDP frame/line shared INT as SMS; Start is polled at port $00, not Pause NMI. EXT link can request NMI $0066; serial/parallel control determines enable. IM0/1/2 and EI delay are Z80 state.",
     "Link peer, external clock, GG/SMS compatibility mode and hardware revision require machine profiles; embedded link model is partial; 12-bit CRAM uses two-byte colour writes"},
};
const DpSegaSystem *dp_sega_system(const char *id) {
    if(!id) return NULL;
    if(!strcmp(id,"sg1000")) id="sg";
    for(unsigned i=0;i<COUNT(systems);i++) if(!strcmp(systems[i].id,id)) return &systems[i];
    return NULL;
}
const DpSegaPort *dp_sega_port(const DpSegaSystem *s,uint16_t port,int write) {
    if(!s) return NULL;
    unsigned count=s->port_count;
    // GG internal ports replace the low-port mirrors, including ignored
    // writes to read-only registers; they do not fall through to SMS gates.
    if(!strcmp(s->id,"gg") && (port&255)<=6) count=7;
    for(unsigned i=0;i<count;i++) {
        const DpSegaPort *p=&s->ports[i];
        if((port&p->mask)==p->value && (p->rw&(write?2:1))) return p;
    }
    return NULL;
}
void dp_sega_regs(FILE *out,const DpSegaSystem *s) {
    fprintf(out,"%s (Z80) hardware registry — metadata, not an execution model\n",s->name);
    fprintf(out,"Memory: %s\nMapper: %s\nInterrupts: %s\n",s->memory,s->mapper,s->interrupts);
    fprintf(out,"RAM: %u bytes; VDP registers: %u; CRAM: %u bytes\nPorts (independent 16-bit IO bus; low-byte decode):\n",s->ram_bytes,s->vdp_registers,s->cram_bytes);
    for(unsigned i=0;i<s->port_count;i++) {
        const DpSegaPort *p=&s->ports[i];
        fprintf(out,"  (port & $%04X) == $%04X  %c%c %-22s %s\n",p->mask,p->value,p->rw&1?'r':'-',p->rw&2?'w':'-',p->name,p->effect);
    }
    static const char *const tms[]={
        "R0: external video, M2 graphics mode",
        "R1: 16 KiB VRAM, display enable, VBlank IRQ enable, M1/M3, sprite size/zoom",
        "R2: name table base", "R3: colour table base/mask (mode dependent)",
        "R4: pattern table base/mask (mode dependent)", "R5: sprite attribute table base",
        "R6: sprite pattern table base", "R7: text foreground and backdrop colour"};
    static const char *const mode4[]={
        "R0: external video, M2/M4, sprite X shift, line IRQ enable, left blanking, H/V scroll locks",
        "R1: display enable, frame IRQ enable, M1/M3 height modes, sprite size/zoom",
        "R2: name table base (alignment/masks depend on mode and VDP revision)",
        "R3: legacy TMS colour table (ignored in mode 4)",
        "R4: legacy TMS pattern table (ignored in mode 4)",
        "R5: sprite attribute table base", "R6: sprite pattern bank",
        "R7: backdrop palette index", "R8: horizontal scroll", "R9: vertical scroll (frame latch)",
        "R10: line IRQ counter reload; pending line IRQ acknowledged with status"};
    const char *const *vdp=!strcmp(s->id,"sg")?tms:mode4;
    fprintf(out,"Indirect VDP registers (through control port; not CPU memory):\n");
    for(unsigned i=0;i<s->vdp_registers;i++) fprintf(out,"  %s\n",vdp[i]);
    fprintf(out,"  Status: bit 7 frame pending, bit 6 sprite overflow, bit 5 sprite collision; read clears flags and control latch. TMS low bits identify fifth sprite.\n");
    fprintf(out,"  SN76489: three 10-bit tone periods, four 4-bit attenuation registers, noise clock/feedback and latch; no IRQ.\n");
    fprintf(out,"Limits: %s\n",s->limitations);
}
