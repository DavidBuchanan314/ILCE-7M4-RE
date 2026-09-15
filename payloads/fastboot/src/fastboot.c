#include "fastboot.h"
#include "usb.h"
#include "io.h"
#include "led.h"

/*
 * Fastboot protocol.
 *
 * The host writes an ASCII command to the bulk OUT endpoint; the device
 * replies on bulk IN with a 4-byte tag and up to 60 bytes of payload:
 *
 *   OKAY<result>          success, ends the command
 *   FAIL<reason>          failure, ends the command
 *   INFO<message>         printed by the host, command continues
 *   DATA<8 hex digits>    device is ready for that many bytes
 *
 * The `oem peek` / `oem poke` commands are the point of this whole exercise:
 * they turn USB into a real debug channel, replacing the LED.
 */

#define FB_RESPONSE_MAX     64
#define FB_COMMAND_MAX      64

#define DMA_SECTION __attribute__((section(".usbdma"), aligned(64)))

/* Bulk buffers must be DMA-visible and OUT transfers get rounded up to a whole
 * number of 512-byte packets, so the command buffer is a full packet. */
static u8 cmd_buf[BULK_MAXPACKET]   DMA_SECTION;
static u8 resp_buf[FB_RESPONSE_MAX] DMA_SECTION;

#define DOWNLOAD_MAX  (512 * 1024)
/*
 * One spare packet of slack. usb_bulk_recv() must round the requested length
 * up to a whole number of maximum-size packets, so the final chunk of a
 * download whose size is not a multiple of 512 can legitimately write past
 * `want` -- up to BULK_MAXPACKET-1 bytes past it. Without the slack that is a
 * buffer overflow for any unaligned download size.
 */
static u8 download_buf[DOWNLOAD_MAX + BULK_MAXPACKET] DMA_SECTION;
static u32 download_len;

/* ---- tiny string helpers (no libc) -------------------------------------- */

static int str_eq(const char *a, const char *b)
{
    while (*a && *b) {
        if (*a != *b)
            return 0;
        a++; b++;
    }
    return *a == *b;
}

/* Does `s` start with `prefix`? Returns the rest of `s`, or 0. */
static const char *str_after(const char *s, const char *prefix)
{
    while (*prefix) {
        if (*s != *prefix)
            return 0;
        s++; prefix++;
    }
    return s;
}

static u32 str_copy(char *dst, const char *src, u32 max)
{
    u32 n = 0;
    while (src[n] && n < max) {
        dst[n] = src[n];
        n++;
    }
    return n;
}

static const char hexdigits[] = "0123456789abcdef";

static u32 hex_format(char *dst, u64 value, int digits)
{
    int i;
    for (i = digits - 1; i >= 0; i--) {
        dst[digits - 1 - i] = hexdigits[(value >> (i * 4)) & 0xf];
    }
    return (u32)digits;
}

/* Parse hex, with or without a 0x prefix. Stops at the first non-hex char and
 * reports where it stopped so callers can find the next field. */
static u64 hex_parse(const char *s, const char **end)
{
    u64 v = 0;

    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
        s += 2;

    for (;;) {
        char c = *s;
        u32 d;

        if (c >= '0' && c <= '9')       d = (u32)(c - '0');
        else if (c >= 'a' && c <= 'f')  d = (u32)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F')  d = (u32)(c - 'A' + 10);
        else break;

        v = (v << 4) | d;
        s++;
    }

    if (end)
        *end = s;
    return v;
}

/* ---- responses ---------------------------------------------------------- */

static void fb_reply(const char *tag, const char *msg)
{
    u32 n = str_copy((char *)resp_buf, tag, 4);
    if (msg)
        n += str_copy((char *)resp_buf + 4, msg, FB_RESPONSE_MAX - 4);
    usb_bulk_send(resp_buf, n);
}

static void fb_okay(const char *msg) { fb_reply("OKAY", msg ? msg : ""); }
static void fb_fail(const char *msg) { fb_reply("FAIL", msg ? msg : ""); }
static void fb_info(const char *msg) { fb_reply("INFO", msg ? msg : ""); }
/*
 * Report a result the user needs to SEE.
 *
 * The fastboot client prints INFO lines (prefixed "(bootloader)") but does not
 * display the payload of an OKAY for `oem` commands -- so anything returned
 * only via fb_okay() is invisible, which silently hid both the exec return
 * value and poke's readback.
 */
static void fb_result(const char *msg)
{
    fb_info(msg);
    fb_okay("");
}

/* ---- oem peek / poke ---------------------------------------------------- */

/*
 * `fastboot oem peek:<addr>[:<len>]`
 *
 * Emits one INFO line per 16 bytes, formatted like a conventional hex dump so
 * it can be read straight out of the fastboot output.
 */
static void cmd_peek(const char *args)
{
    const char *p;
    u64 addr;
    u32 len, off;
    char line[FB_RESPONSE_MAX];

    addr = hex_parse(args, &p);
    len  = 4;
    if (*p == ':')
        len = (u32)hex_parse(p + 1, 0);

    if (len == 0 || len > 0x1000) {
        fb_fail("bad length (max 0x1000)");
        return;
    }

    /* Registers are 32-bit; an unaligned MMIO read would fault or return
     * nonsense, so force alignment rather than silently misreporting. */
    addr &= ~3ull;
    len = (len + 3) & ~3u;

    for (off = 0; off < len; off += 16) {
        u32 n = 0;
        u32 i;

        n += hex_format(line + n, addr + off, 8);
        line[n++] = ':';

        for (i = 0; i < 16 && off + i < len; i += 4) {
            u32 word = read32(addr + off + i);
            line[n++] = ' ';
            n += hex_format(line + n, word, 8);
        }
        line[n] = 0;
        fb_info(line);
    }

    fb_okay("");
}

/* `fastboot oem poke:<addr>:<value>` -- single 32-bit write. */
static void cmd_poke(const char *args)
{
    const char *p;
    u64 addr;
    u32 value;
    char line[FB_RESPONSE_MAX];
    u32 n = 0;

    addr = hex_parse(args, &p);
    if (*p != ':') {
        fb_fail("usage: oem poke:<addr>:<value>");
        return;
    }
    value = (u32)hex_parse(p + 1, 0);

    addr &= ~3ull;
    write32(addr, value);
    dsb();

    /* Read back: on MMIO this is often not the value written, and that
     * difference is usually the interesting part. */
    n += str_copy(line + n, "wrote ", FB_RESPONSE_MAX);
    n += hex_format(line + n, value, 8);
    n += str_copy(line + n, " -> ", 8);
    n += hex_format(line + n, addr, 8);
    n += str_copy(line + n, " readback ", 16);
    n += hex_format(line + n, read32(addr), 8);
    line[n] = 0;
    fb_result(line);
}

/* ---- oem exec ----------------------------------------------------------- */

/*
 * Make instruction fetches see memory that was written by something other than
 * the CPU's own stores -- i.e. anything the USB controller DMA'd in.
 *
 * With the MMU off, data accesses are Device-nGnRnE (uncached), so the D-side
 * needs nothing. Instruction fetches are a separate question: SCTLR.I is
 * independent of SCTLR.M, so the I-cache can be live even with the MMU off,
 * and it may hold stale lines for an address we just downloaded into.
 * IC IALLU invalidates the lot, which costs nothing here and avoids having to
 * know the length of the code being run.
 */
static inline void sync_icache(void)
{
    __asm__ volatile(
        "dsb sy\n"
        "ic  iallu\n"
        "dsb sy\n"
        "isb\n"
        ::: "memory");
}

/*
 * `fastboot oem exec:<addr>` -- call addr as a function and report what it
 * returns.
 *
 * The callee runs at EL3 with our stack and is trusted to obey the AAPCS64
 * calling convention. If it clobbers callee-saved registers or the stack, or
 * simply never returns, this payload is gone and the camera needs a re-push;
 * that is inherent to the command, not a bug.
 */
static void cmd_exec(const char *args)
{
    const char *p;
    u64 addr = hex_parse(args, &p);
    u64 ret;
    char line[FB_RESPONSE_MAX];
    u32 n = 0;

    if (p == args) {
        fb_fail("usage: oem exec:<addr>");
        return;
    }
    /* AArch64 instructions are word-aligned; branching elsewhere is a PC
     * alignment fault, which from here is an invisible hang. */
    if (addr & 3) {
        fb_fail("address must be 4-byte aligned");
        return;
    }

    /* Announce BEFORE jumping, so a callee that never returns looks different
     * from a command that was rejected. */
    n = str_copy(line, "calling ", FB_RESPONSE_MAX);
    n += hex_format(line + n, addr, 16);
    line[n] = 0;
    fb_info(line);

    sync_icache();

    ret = ((u64 (*)(void))(unsigned long)addr)();

    n = str_copy(line, "returned ", FB_RESPONSE_MAX);
    n += hex_format(line + n, ret, 16);
    line[n] = 0;
    fb_result(line);
}

/* ---- oem help ----------------------------------------------------------- */

static void cmd_help(void)
{
    char line[FB_RESPONSE_MAX];
    u32 n;

    fb_info("help                 list oem commands");
    fb_info("peek:<addr>[:<len>]  dump memory, max len 0x1000");
    fb_info("poke:<addr>:<val>    32-bit write, reports readback");
    fb_info("exec:<addr>          call addr, reports return value");

    n = str_copy(line, "download buffer ", FB_RESPONSE_MAX);
    n += hex_format(line + n, (u64)(unsigned long)download_buf, 8);
    n += str_copy(line + n, " len ", FB_RESPONSE_MAX - n);
    n += hex_format(line + n, DOWNLOAD_MAX, 8);
    line[n] = 0;
    fb_info(line);

    fb_okay("");
}

static void cmd_oem(const char *args)
{
    const char *rest;

    if (str_eq(args, "help")) {
        cmd_help();
        return;
    }
    if ((rest = str_after(args, "peek:")) != 0) {
        cmd_peek(rest);
        return;
    }
    if ((rest = str_after(args, "poke:")) != 0) {
        cmd_poke(rest);
        return;
    }
    if ((rest = str_after(args, "exec:")) != 0) {
        cmd_exec(rest);
        return;
    }
    fb_fail("unknown oem command (try: oem help)");
}

/* ---- getvar ------------------------------------------------------------- */

static void cmd_getvar(const char *name)
{
    char line[FB_RESPONSE_MAX];
    u32 n;

    if (str_eq(name, "version")) {
        fb_okay("0.4");
    } else if (str_eq(name, "product")) {
        fb_okay("ILCE-7M4");
    } else if (str_eq(name, "serialno")) {
        fb_okay("ILCE7M4-fastboot");
    } else if (str_eq(name, "max-download-size")) {
        n = str_copy(line, "0x", FB_RESPONSE_MAX);
        n += hex_format(line + n, DOWNLOAD_MAX, 8);
        line[n] = 0;
        fb_okay(line);
    } else if (str_eq(name, "downloadsize")) {
        n = str_copy(line, "0x", FB_RESPONSE_MAX);
        n += hex_format(line + n, DOWNLOAD_MAX, 8);
        line[n] = 0;
        fb_okay(line);
    } else {
        /* Unknown variables must answer OKAY with an empty value, not FAIL --
         * `fastboot getvar all` walks a list and a FAIL aborts the walk. */
        fb_okay("");
    }
}

/* ---- download ----------------------------------------------------------- */

static void cmd_download(const char *args)
{
    char line[16];
    u32 want = (u32)hex_parse(args, 0);
    u32 got = 0;
    u32 n;

    if (want == 0 || want > DOWNLOAD_MAX) {
        fb_fail("data too large");
        return;
    }

    /* DATA<8 hex> tells the host to start sending. */
    n = str_copy(line, "DATA", 4);
    n += hex_format(line + n, want, 8);
    usb_bulk_send(line, n);

    while (got < want) {
        u32 chunk = want - got;
        if (chunk > BULK_MAXPACKET * 8)
            chunk = BULK_MAXPACKET * 8;
        got += usb_bulk_recv(download_buf + got, chunk);
    }

    download_len = want;
    fb_okay("");
}

/* ---- command dispatch --------------------------------------------------- */

static void fastboot_command(const char *cmd)
{
    const char *rest;

    if ((rest = str_after(cmd, "getvar:")) != 0) {
        cmd_getvar(rest);
    } else if ((rest = str_after(cmd, "download:")) != 0) {
        cmd_download(rest);
    } else if ((rest = str_after(cmd, "oem ")) != 0) {
        cmd_oem(rest);
    } else if (str_eq(cmd, "reboot") || str_eq(cmd, "reboot-bootloader")) {
        /* Nothing sane to reboot into from a bootrom payload; answer so the
         * host does not hang waiting. */
        fb_okay("");
    } else {
        fb_fail("unknown command");
    }
}

void fastboot_loop(void)
{
    (void)download_len;

    for (;;) {
        u32 n;

        usb_state = USB_STATE_FB_LOOP_TOP;

        /* Deferred from SET_CONFIGURATION; safe to do here. */
        usb_bulk_enable_pending();

        /*
         * If the bulk path is broken, keep servicing control traffic rather
         * than stopping. led_fail() used to be called here, and because it
         * loops forever without pumping events it took ep0 down with it --
         * killing the status-descriptor channel that exists to diagnose this
         * exact situation. The error is reported through string descriptor 4.
         */
        if (usb_bulk_error) {
            usb_event_pump();
            continue;
        }

        n = usb_bulk_recv(cmd_buf, FB_COMMAND_MAX);

        if (n == 0)
            continue;
        if (n >= sizeof(cmd_buf))
            n = sizeof(cmd_buf) - 1;
        cmd_buf[n] = 0;

        fastboot_command((const char *)cmd_buf);
    }
}
