#include "fastboot.h"
#include "usb.h"
#include "io.h"
#include "led.h"
#include "sdhci.h"

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

/* ---- oem partition ------------------------------------------------------ */

#define SECTOR_SIZE         512
/*
 * The SDM2 table starts at byte 32 with 16 bytes per entry. Four sectors holds
 * 126 entries, comfortably more than the ~23 this device uses, and costs one
 * read either way.
 */
#define PT_SECTORS          4
#define PT_MAX_ENTRIES      ((PT_SECTORS * SECTOR_SIZE - 32) / 16)

/*
 * eMMC boot partition size. The hardware answer is EXT_CSD[226] BOOT_SIZE_MULT
 * x 128 KiB, but the ROM exposes no way to read EXT_CSD, so this is the
 * observed size of the nflashaB0 dump (4 MiB = 0x2000 sectors). Boot0 and
 * boot1 are always the same size as each other.
 */
#define BOOT_PART_SECTORS   0x2000

static u8 pt_buf[PT_SECTORS * SECTOR_SIZE] DMA_SECTION;

static u32 le32(const u8 *p)
{
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

static u32 dec_format(char *dst, u32 v)
{
    char tmp[12];
    u32 n = 0, i = 0;

    if (v == 0) {
        dst[0] = '0';
        return 1;
    }
    while (v) {
        tmp[n++] = (char)('0' + (v % 10));
        v /= 10;
    }
    while (n)
        dst[i++] = tmp[--n];
    return i;
}

/* "<name>  <start8>  <count8>  <type>", one INFO line per partition. */
static void pt_line(const char *name, u32 start, u32 count, const char *type)
{
    char line[FB_RESPONSE_MAX];
    u32 n = 0;

    n += str_copy(line + n, name, 16);
    while (n < 11)
        line[n++] = ' ';
    n += hex_format(line + n, start, 8);
    line[n++] = ' ';
    n += hex_format(line + n, count, 8);
    line[n++] = ' ';
    n += str_copy(line + n, type, 8);
    line[n] = 0;
    fb_info(line);
}

/*
 * Column header. Written as its own function rather than by passing zeros
 * through pt_line(), which printed the start/count headings as "00000000".
 * The padding targets match pt_line's layout exactly: name at 0, start at 11,
 * count at 20, type at 29.
 */
static void pt_header(void)
{
    char line[FB_RESPONSE_MAX];
    u32 n = 0;

    n += str_copy(line + n, "device", 16);
    while (n < 11)
        line[n++] = ' ';
    n += str_copy(line + n, "start", 8);
    while (n < 20)
        line[n++] = ' ';
    n += str_copy(line + n, "count", 8);
    while (n < 29)
        line[n++] = ' ';
    n += str_copy(line + n, "type", 8);
    line[n] = 0;
    fb_info(line);
}

static const char *pt_typename(u32 type)
{
    switch (type) {
    case 4: return "ext2";
    case 5: return "fat";
    case 6: return "wbi2";
    }
    return "?";
}

/*
 * Armed streaming source for `upload`. Nothing is buffered: `oem partition
 * dump` only records what to send, and the upload streams straight off the
 * eMMC a chunk at a time. That is what removes the size limit -- a 681 MB
 * partition needs no more memory than a 4 MB one.
 */
static struct {
    u32 dev;            /* eMMC PARTITION_ACCESS */
    u32 start;          /* absolute start sector */
    u32 sectors;        /* how many to send */
} upload_src;

#define DUMP_CHUNK_SECTORS  (64 * 1024 / SECTOR_SIZE)   /* 64 KiB per read */

/* Copy the next whitespace-delimited token, returning where it stopped. */
static const char *parse_token(const char *s, char *out, u32 max)
{
    u32 n = 0;

    while (*s == ' ')
        s++;
    while (*s && *s != ' ' && n + 1 < max)
        out[n++] = *s++;
    out[n] = 0;
    return s;
}

/* Read the SDM2 table into pt_buf. Returns MMC_OK, or a negative error. */
static int pt_load(void)
{
    int rc;

    if (!mmc_ready)
        mmc_init();
    if (!mmc_ready)
        return MMC_ERR_CMD_TIMEOUT;

    rc = mmc_select_partition(0);
    if (rc == MMC_OK)
        rc = mmc_read_blocks(0, pt_buf, PT_SECTORS);
    if (rc != MMC_OK)
        return rc;

    dsb();
    if (pt_buf[0] != '8' || pt_buf[1] != '2' ||
        pt_buf[2] != '4' || pt_buf[3] != '6')
        return MMC_ERR_XFER;
    return MMC_OK;
}

/*
 * Resolve a device name from the listing to (dev, start, sectors).
 *
 *   nflashaB0 / nflashaB1  the eMMC boot hardware partitions (dev 1 / 2)
 *   nflasha                the whole user area, sized by the table's extent
 *   nflashaN               slot N of the SDM2 table, 1-indexed as the dump
 *                          script names them
 */
static int resolve_part(const char *name, u32 *dev, u32 *start, u32 *sectors)
{
    const char *tail = str_after(name, "nflasha");
    u32 want, i, n_part, extent = 0;
    int rc;

    if (!tail)
        return -1;

    if (str_eq(tail, "B0")) {
        *dev = 1; *start = 0; *sectors = BOOT_PART_SECTORS;
        return 0;
    }
    if (str_eq(tail, "B1")) {
        *dev = 2; *start = 0; *sectors = BOOT_PART_SECTORS;
        return 0;
    }

    rc = pt_load();
    if (rc != MMC_OK)
        return rc;

    n_part = le32(pt_buf + 8);
    if (n_part > PT_MAX_ENTRIES)
        n_part = PT_MAX_ENTRIES;

    if (*tail == 0) {                   /* bare "nflasha": whole user area */
        for (i = 0; i < n_part; i++) {
            const u8 *e = pt_buf + 32 + i * 16;
            u32 end;

            if (!(le32(e + 12) & 1))
                continue;
            end = le32(e) + le32(e + 4);
            if (end > extent)
                extent = end;
        }
        *dev = 0; *start = 0; *sectors = extent;
        return 0;
    }

    /* nflashaN -- decimal slot number. */
    want = 0;
    while (*tail >= '0' && *tail <= '9')
        want = want * 10 + (u32)(*tail++ - '0');
    if (*tail != 0 || want == 0 || want > n_part)
        return -1;

    {
        const u8 *e = pt_buf + 32 + (want - 1) * 16;

        if (!(le32(e + 12) & 1))
            return -1;                  /* slot exists but is not valid */
        *dev = 0;
        *start = le32(e);
        *sectors = le32(e + 4);
    }
    return 0;
}

/* `oem partition dump <name> [<offset> [<size>]]`, offset/size in hex sectors. */
static void cmd_partition_dump(const char *args)
{
    char name[24], line[FB_RESPONSE_MAX];
    const char *p;
    u32 dev = 0, start = 0, sectors = 0, offset = 0, want, n;
    int rc;

    p = parse_token(args, name, sizeof(name));
    if (name[0] == 0) {
        fb_fail("usage: oem partition dump <name> [<off> [<size>]]");
        return;
    }

    rc = resolve_part(name, &dev, &start, &sectors);
    if (rc != 0) {
        n = str_copy(line, "cannot resolve ", FB_RESPONSE_MAX);
        n += str_copy(line + n, name, 24);
        line[n] = 0;
        fb_fail(line);
        return;
    }

    while (*p == ' ')
        p++;
    if (*p)
        offset = (u32)hex_parse(p, &p);
    while (*p == ' ')
        p++;
    want = *p ? (u32)hex_parse(p, 0) : (sectors > offset ? sectors - offset : 0);

    if (offset >= sectors || want == 0) {
        fb_fail("offset past end of partition");
        return;
    }
    if (want > sectors - offset)
        want = sectors - offset;

    upload_src.dev = dev;
    upload_src.start = start + offset;
    upload_src.sectors = want;

    /* The arming is silent; the only thing worth saying is what to run next. */
    n = str_copy(line, "hint: fastboot get_staged ", FB_RESPONSE_MAX);
    n += str_copy(line + n, name, sizeof(name));
    n += str_copy(line + n, ".bin", 8);
    line[n] = 0;
    fb_result(line);
}

/*
 * `upload` -- what `fastboot get_staged` issues.
 *
 * Announce the total with DATA<8hex>, then stream it. The host only cares that
 * exactly that many bytes arrive, so they can be produced incrementally.
 */
static void cmd_upload(void)
{
    char line[16];
    u32 done = 0, n;

    if (upload_src.sectors == 0) {
        fb_fail("nothing armed (try: oem partition dump <name>)");
        return;
    }

    n = str_copy(line, "DATA", 4);
    n += hex_format(line + n, upload_src.sectors * SECTOR_SIZE, 8);
    usb_bulk_send(line, n);

    if (mmc_select_partition(upload_src.dev) != MMC_OK) {
        /* Too late for FAIL: the host is already reading the data phase. */
        return;
    }

    while (done < upload_src.sectors) {
        u32 chunk = upload_src.sectors - done;

        if (chunk > DUMP_CHUNK_SECTORS)
            chunk = DUMP_CHUNK_SECTORS;

        if (mmc_read_blocks(upload_src.start + done, download_buf,
                            chunk) != MMC_OK)
            return;

        usb_bulk_send(download_buf, chunk * SECTOR_SIZE);
        done += chunk;
    }

    fb_okay("");
}

static void cmd_partition(void)
{
    char name[16];
    char line[FB_RESPONSE_MAX];
    u32 n_part, i, extent = 0, n;
    int rc, init_rc = 0;

    pt_header();

    /*
     * The two eMMC boot hardware partitions. They are not in the SDM2 table --
     * that table describes the user area only -- so they are listed from what
     * the hardware layout guarantees: each starts at its own sector 0.
     */
    pt_line("nflashaB0", 0, BOOT_PART_SECTORS, "raw");
    pt_line("nflashaB1", 0, BOOT_PART_SECTORS, "raw");

    /* SDM2 table: sector 0 of the user area. */
    /* Startup init may have failed (card asleep, powered late). Retry now
     * rather than reporting a stale failure. */
    if (!mmc_ready)
        init_rc = mmc_init();

    if (!mmc_ready) {
        /*
         * Identification is the failure, so report what mmc_init() knows --
         * which step died, the OCR it got, the controller error. Without this
         * the message below would say only that the read failed, discarding
         * the part that actually says why. mmc_init_step: 1 CMD0, 2 CMD1,
         * 3 CMD2, 4 CMD3, 5 CMD7, 6 BUS_WIDTH, 7 HS_TIMING.
         */
        n = str_copy(line, "mmc init rc ", FB_RESPONSE_MAX);
        n += hex_format(line + n, (u32)init_rc, 2);
        n += str_copy(line + n, " step ", 8);
        n += dec_format(line + n, mmc_init_step);
        n += str_copy(line + n, " err ", 8);
        n += hex_format(line + n, mmc_last_error, 4);
        n += str_copy(line + n, " ocr ", 8);
        n += hex_format(line + n, mmc_ocr, 8);
        line[n] = 0;
        fb_fail(line);
        return;
    }

    /* The SDM2 table exists only in the user area -- boot0 holds the EXBL
     * Information Sector and boot1 is blank -- so there is nothing to select. */
    rc = mmc_select_partition(0);
    if (rc == MMC_OK)
        rc = mmc_read_blocks(0, pt_buf, PT_SECTORS);
    dsb();

    if (rc != MMC_OK || pt_buf[0] != '8' || pt_buf[1] != '2' ||
        pt_buf[2] != '4' || pt_buf[3] != '6') {
        n = str_copy(line, "rc ", FB_RESPONSE_MAX);
        n += hex_format(line + n, (u32)rc, 2);
        n += str_copy(line + n, " err ", 8);
        n += hex_format(line + n, mmc_last_error, 4);
        n += str_copy(line + n, " magic ", 8);
        n += hex_format(line + n, le32(pt_buf), 8);
        line[n] = 0;
        fb_fail(line);
        return;
    }

    n_part = le32(pt_buf + 8);
    if (n_part > PT_MAX_ENTRIES)
        n_part = PT_MAX_ENTRIES;

    /*
     * The bare "nflasha" device is the whole user area -- the raw disk the
     * dump script reads, before any partition is carved out of it.
     *
     * Its true size is EXT_CSD[212] SEC_COUNT, which the ROM gives us no way
     * to read, so what is printed is the extent the partition table actually
     * covers: the highest start+count over valid entries. That is a lower
     * bound on the device size, not the device size, and the summary line says
     * so rather than letting the number be mistaken for the real capacity.
     */
    for (i = 0; i < n_part; i++) {
        const u8 *e = pt_buf + 32 + i * 16;
        u32 end;

        if (!(le32(e + 12) & 1))
            continue;
        end = le32(e) + le32(e + 4);
        if (end > extent)
            extent = end;
    }
    pt_line("nflasha", 0, extent, "raw");

    for (i = 0; i < n_part; i++) {
        const u8 *e = pt_buf + 32 + i * 16;
        u32 start = le32(e);
        u32 count = le32(e + 4);
        u32 type  = le32(e + 8);
        u32 flag  = le32(e + 12);

        /* SDM_LABEL_VALID. Invalid entries are skipped but still consume an
         * index, because the device name is derived from the slot, not from
         * the position in the printed list. */
        if (!(flag & 1))
            continue;

        n = str_copy(name, "nflasha", sizeof(name));
        n += dec_format(name + n, i + 1);   /* 1-indexed, as the dump script does */
        name[n] = 0;

        pt_line(name, start, count, pt_typename(type));
    }

    fb_okay("");
}

/*
 * `oem mmccmd:<idx>:<arg>[:<flags>]` -- issue one raw eMMC command.
 *
 * Flags default to a 48-bit response with CRC and index checking (R1). Pass
 * them explicitly for other response types: 0 none, 3 R1b, 1 long/R2.
 * Reports the controller return code, the Error Interrupt Status, the R1
 * response and the card state decoded out of it.
 */
static void cmd_mmccmd(const char *args)
{
    const char *p;
    char line[FB_RESPONSE_MAX];
    u32 idx, arg = 0, resp = 0, n = 0;
    u16 flags = SDHCI_CMD_RESP_SHORT | SDHCI_CMD_CRC_CHECK |
                SDHCI_CMD_INDEX_CHECK;
    int rc;

    idx = (u32)hex_parse(args, &p);
    if (*p == ':')
        arg = (u32)hex_parse(p + 1, &p);
    if (*p == ':')
        flags = (u16)hex_parse(p + 1, 0);

    rc = mmc_raw_cmd((u8)idx, arg, flags, &resp);

    n = str_copy(line, "cmd ", FB_RESPONSE_MAX);
    n += dec_format(line + n, idx);
    n += str_copy(line + n, " rc ", 8);
    n += hex_format(line + n, (u32)rc, 2);
    n += str_copy(line + n, " err ", 8);
    n += hex_format(line + n, mmc_last_error, 4);
    n += str_copy(line + n, " resp ", 8);
    n += hex_format(line + n, resp, 8);
    line[n] = 0;
    fb_info(line);

    /* R1 bits [12:9] are CURRENT_STATE: 0 idle, 1 ready, 2 ident, 3 stby,
     * 4 tran, 5 data, 6 rcv, 7 prg, 8 dis. */
    n = str_copy(line, "state ", FB_RESPONSE_MAX);
    n += dec_format(line + n, (resp >> 9) & 0xf);
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
    fb_info("help                 list oem commands");
    fb_info("peek:<addr>[:<len>]  dump memory, max len 0x1000");
    fb_info("poke:<addr>:<val>    32-bit write, reports readback");
    fb_info("exec:<addr>          call addr, reports return value");
    fb_info("partition            list eMMC partitions");
    fb_info("partition dump <name> [<off> [<sz>]]");
    fb_info("mmccmd:<i>:<a>[:<f>] raw eMMC command");

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
    if ((rest = str_after(args, "mmccmd:")) != 0) {
        cmd_mmccmd(rest);
        return;
    }
    if ((rest = str_after(args, "partition dump ")) != 0) {
        cmd_partition_dump(rest);
        return;
    }
    if (str_eq(args, "partition")) {
        cmd_partition();
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
    } else if (str_eq(cmd, "upload")) {
        cmd_upload();
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
