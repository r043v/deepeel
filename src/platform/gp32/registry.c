// registry.c: the GP32 (Samsung S3C2400X01) hardware registry: every special
// function register the SoC decodes, the 32 interrupt sources and the memory
// windows.  The 32-bit bus needs find().  First match wins.
#include "../platform.h"
#include "gp32_platform.h"

#define R(a, n, c)       { (a), (a) + 3, n, c, 3, DP_IMPL_MODELLED }
#define RI(a, n, c, i)   { (a), (a) + 3, n, c, 3, (i) | DP_IMPL_MODELLED }
#define DMA(n, b) \
    RI((b) + 0x00, "DISRC" n, "dma", DP_IMPL_DMA_SRC), R((b) + 0x04, "DIDST" n, "dma"), R((b) + 0x08, "DCON" n, "dma"), \
    R((b) + 0x0C, "DSTAT" n, "dma"), R((b) + 0x10, "DCSRC" n, "dma"), R((b) + 0x14, "DCDST" n, "dma"), \
    RI((b) + 0x18, "DMASKTRIG" n, "dma", DP_IMPL_DMA_SRC)
#define UART(n, b) \
    R((b) + 0x00, "ULCON" n, "serial"), R((b) + 0x04, "UCON" n, "serial"), R((b) + 0x08, "UFCON" n, "serial"), \
    R((b) + 0x0C, "UMCON" n, "serial"), RI((b) + 0x10, "UTRSTAT" n, "serial", DP_IMPL_EVENT), \
    R((b) + 0x14, "UERSTAT" n, "serial"), R((b) + 0x18, "UFSTAT" n, "serial"), R((b) + 0x1C, "UMSTAT" n, "serial"), \
    R((b) + 0x20, "UTXH" n, "serial"), R((b) + 0x24, "URXH" n, "serial"), R((b) + 0x28, "UBRDIV" n, "serial")
#define TIMER(n, b) \
    R((b) + 0, "TCNTB" n, "timer"), R((b) + 4, "TCMPB" n, "timer"), RI((b) + 8, "TCNTO" n, "timer", DP_IMPL_EVENT)

static const DpIoReg gp32_io[] = {
    R(0x14000000, "BWSCON", "memory"), R(0x14000004, "BANKCON0", "memory"), R(0x14000008, "BANKCON1", "memory"),
    R(0x1400000C, "BANKCON2", "memory"), R(0x14000010, "BANKCON3", "memory"), R(0x14000014, "BANKCON4", "memory"),
    R(0x14000018, "BANKCON5", "memory"), R(0x1400001C, "BANKCON6", "memory"), R(0x14000020, "BANKCON7", "memory"),
    R(0x14000024, "REFRESH", "memory"), R(0x14000028, "BANKSIZE", "memory"), R(0x1400002C, "MRSRB6", "memory"),
    R(0x14000030, "MRSRB7", "memory"),
    RI(0x14400000, "SRCPND", "interrupt", DP_IMPL_IRQ), RI(0x14400004, "INTMOD", "interrupt", DP_IMPL_IRQ),
    RI(0x14400008, "INTMSK", "interrupt", DP_IMPL_IRQ), RI(0x1440000C, "PRIORITY", "interrupt", DP_IMPL_IRQ),
    RI(0x14400010, "INTPND", "interrupt", DP_IMPL_IRQ), RI(0x14400014, "INTOFFSET", "interrupt", DP_IMPL_IRQ),
    DMA("0", 0x14600000), DMA("1", 0x14600020), DMA("2", 0x14600040), DMA("3", 0x14600060),
    R(0x14800000, "LOCKTIME", "sys"), R(0x14800004, "MPLLCON", "sys"), R(0x14800008, "UPLLCON", "sys"),
    R(0x1480000C, "CLKCON", "sys"), R(0x14800010, "CLKSLOW", "sys"), R(0x14800014, "CLKDIVN", "sys"),
    R(0x14A00000, "LCDCON1", "video"), R(0x14A00004, "LCDCON2", "video"), R(0x14A00008, "LCDCON3", "video"),
    R(0x14A0000C, "LCDCON4", "video"), RI(0x14A00010, "LCDCON5", "video", DP_IMPL_EVENT),
    RI(0x14A00014, "LCDSADDR1", "video", DP_IMPL_DMA_SRC), R(0x14A00018, "LCDSADDR2", "video"),
    R(0x14A0001C, "LCDSADDR3", "video"), R(0x14A00020, "REDLUT", "video"), R(0x14A00024, "GREENLUT", "video"),
    R(0x14A00028, "BLUELUT", "video"), { 0x14A0002C, 0x14A0004B, "LCDRSVD", "video", 3, 0 },
    R(0x14A0004C, "DITHMODE", "video"), R(0x14A00050, "TPAL", "video"),
    { 0x14A00400, 0x14A007FF, "PALETTE", "video", 3, DP_IMPL_WINDOW | DP_IMPL_MODELLED },
    UART("0", 0x15000000), UART("1", 0x15004000),
    R(0x15100000, "TCFG0", "timer"), R(0x15100004, "TCFG1", "timer"), R(0x15100008, "TCON", "timer"),
    TIMER("0", 0x1510000C), TIMER("1", 0x15100018), TIMER("2", 0x15100024), TIMER("3", 0x15100030),
    R(0x1510003C, "TCNTB4", "timer"), RI(0x15100040, "TCNTO4", "timer", DP_IMPL_EVENT),
    { 0x15200000, 0x1520013F, "USBHOST", "usb", 3, 0 }, { 0x15200140, 0x1520033F, "USBDEV", "usb", 3, DP_IMPL_MODELLED },
    R(0x15300000, "WTCON", "sys"), R(0x15300004, "WTDAT", "sys"), RI(0x15300008, "WTCNT", "sys", DP_IMPL_EVENT),
    R(0x15400000, "IICCON", "eeprom"), R(0x15400004, "IICSTAT", "eeprom"), R(0x15400008, "IICADD", "eeprom"),
    R(0x1540000C, "IICDS", "eeprom"),
    R(0x15508000, "IISCON", "sound"), R(0x15508004, "IISMOD", "sound"), R(0x15508008, "IISPSR", "sound"),
    R(0x1550800C, "IISFCON", "sound"), R(0x15508010, "IISFIFO", "sound"),
    R(0x15600000, "PACON", "gpio"), R(0x15600004, "PADAT", "gpio"), R(0x15600008, "PBCON", "gpio"),
    RI(0x1560000C, "PBDAT", "input", DP_IMPL_EVENT), R(0x15600010, "PBUP", "gpio"), R(0x15600014, "PCCON", "gpio"),
    R(0x15600018, "PCDAT", "gpio"), R(0x1560001C, "PCUP", "gpio"), R(0x15600020, "PDCON", "gpio"),
    R(0x15600024, "PDDAT", "smartmedia"), R(0x15600028, "PDUP", "gpio"), R(0x1560002C, "PECON", "gpio"),
    RI(0x15600030, "PEDAT", "input", DP_IMPL_EVENT), R(0x15600034, "PEUP", "gpio"), R(0x15600038, "PFCON", "gpio"),
    R(0x1560003C, "PFDAT", "gpio"), R(0x15600040, "PFUP", "gpio"), R(0x15600044, "PGCON", "gpio"),
    R(0x15600048, "PGDAT", "gpio"), R(0x1560004C, "PGUP", "gpio"), R(0x15600050, "OPENCR", "gpio"),
    R(0x15600054, "MISCCR", "gpio"), RI(0x15600058, "EXTINT", "interrupt", DP_IMPL_IRQ),
    R(0x15700040, "RTCCON", "rtc"), RI(0x15700044, "TICNT", "rtc", DP_IMPL_IRQ), R(0x15700050, "RTCALM", "rtc"),
    R(0x15700054, "ALMSEC", "rtc"), R(0x15700058, "ALMMIN", "rtc"), R(0x1570005C, "ALMHOUR", "rtc"),
    R(0x15700060, "ALMDATE", "rtc"), R(0x15700064, "ALMMON", "rtc"), R(0x15700068, "ALMYEAR", "rtc"),
    R(0x1570006C, "RTCRST", "rtc"), RI(0x15700070, "BCDSEC", "rtc", DP_IMPL_EVENT),
    RI(0x15700074, "BCDMIN", "rtc", DP_IMPL_EVENT), RI(0x15700078, "BCDHOUR", "rtc", DP_IMPL_EVENT),
    RI(0x1570007C, "BCDDATE", "rtc", DP_IMPL_EVENT), RI(0x15700080, "BCDDAY", "rtc", DP_IMPL_EVENT),
    RI(0x15700084, "BCDMON", "rtc", DP_IMPL_EVENT), RI(0x15700088, "BCDYEAR", "rtc", DP_IMPL_EVENT),
    R(0x15800000, "ADCCON", "analog"), R(0x15800004, "ADCDAT", "analog"),
    R(0x15900000, "SPCON", "serial"), R(0x15900004, "SPSTA", "serial"), R(0x15900008, "SPPIN", "serial"),
    R(0x1590000C, "SPPRE", "serial"), R(0x15900010, "SPTDAT", "serial"), R(0x15900014, "SPRDAT", "serial"),
    { 0x15A00000, 0x15A00043, "MMC", "mmc", 3, DP_IMPL_MODELLED },
};
#define GP32_IO_N (int)(sizeof gp32_io / sizeof gp32_io[0])

#define Q(b, n) { (b), 0x18, n }
static const DpIrq gp32_irqs[] = {
    Q(0, "EINT0"), Q(1, "EINT1"), Q(2, "EINT2"), Q(3, "EINT3"), Q(4, "EINT4"), Q(5, "EINT5"), Q(6, "EINT6"), Q(7, "EINT7"),
    Q(8, "TICK"), Q(9, "WDT"), Q(10, "TIMER0"), Q(11, "TIMER1"), Q(12, "TIMER2"), Q(13, "TIMER3"), Q(14, "TIMER4"),
    Q(15, "UERR"), Q(17, "DMA0"), Q(18, "DMA1"), Q(19, "DMA2"), Q(20, "DMA3"), Q(21, "MMC"), Q(22, "SPI"),
    Q(23, "URXD0"), Q(24, "URXD1"), Q(25, "USBD"), Q(26, "USBH"), Q(27, "IIC"), Q(28, "UTXD0"), Q(29, "UTXD1"),
    Q(30, "RTC"), Q(31, "ADC"),
};

static int gp32_find(uint32_t a) {
    if (a < 0x14000000u || a > 0x15A00043u) return -1;
    int lo = 0, hi = GP32_IO_N - 1;   // the table is sorted by address
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (a < gp32_io[mid].lo) hi = mid - 1;
        else if (a > gp32_io[mid].hi) lo = mid + 1;
        else return mid;
    }
    return -1;
}

const DpPlatform *dp_platform_gp32(void) {
    static const DpPlatform p = {
        .name = "gp32", .io_regs = gp32_io, .io_reg_count = GP32_IO_N,
        .irqs = gp32_irqs, .irq_count = (int)(sizeof gp32_irqs / sizeof gp32_irqs[0]), .find = gp32_find,
    };
    return &p;
}
