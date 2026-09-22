#include "fastboot.h"
#include "usb.h"
#include "io.h"
#include "led.h"
#include "sdhci.h"
#include "spacc.h"
#include "dwc3.h"
#include "reset.h"
#include "darwin.h"
#include "cetus.h"
#include "log.h"

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
 */

#define FB_RESPONSE_MAX     64
#define FB_TAG_LEN          4       /* OKAY / FAIL / INFO / DATA */
#define FB_COMMAND_MAX      64

#define DMA_SECTION __attribute__((section(".usbdma"), aligned(64)))

/* Bulk buffers must be DMA-visible and OUT transfers get rounded up to a whole
 * number of 512-byte packets, so the command buffer is a full packet. */
static u8 cmd_buf[BULK_MAXPACKET]   DMA_SECTION;
static u8 resp_buf[FB_RESPONSE_MAX] DMA_SECTION;

#define DOWNLOAD_MAX  (512 * 1024)
/*
 * usb_bulk_recv() rounds up to a whole packet, so the last chunk can write up
 * to BULK_MAXPACKET-1 bytes past the requested length.
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

/* Append src at dst[n], bounded by the whole buffer. Returns the new length. */
static u32 str_append(char *dst, u32 n, u32 size, const char *src)
{
    while (*src && n + 1 < size)
        dst[n++] = *src++;
    return n;
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

/* Hex with no leading zeros. */
static u32 hex_format_min(char *dst, u64 value)
{
    int digits = 1, i;
    u64 t = value;

    while (t >>= 4)
        digits++;
    for (i = 0; i < digits; i++)
        dst[i] = hexdigits[(value >> ((digits - 1 - i) * 4)) & 0xf];
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
    u32 n = str_copy((char *)resp_buf, tag, FB_TAG_LEN);
    if (msg)
        n = str_append((char *)resp_buf, n, sizeof(resp_buf), msg);
    usb_bulk_send(resp_buf, n);
}

static void fb_okay(const char *msg) { fb_reply("OKAY", msg ? msg : ""); }
static void fb_fail(const char *msg) { fb_reply("FAIL", msg ? msg : ""); }
static void fb_info(const char *msg) { fb_reply("INFO", msg ? msg : ""); }
/*
 * The fastboot client prints INFO lines but not the payload of an OKAY for
 * `oem` commands, so a result returned only via fb_okay() is invisible.
 */
static void fb_result(const char *msg)
{
    fb_info(msg);
    fb_okay("");
}

/* ---- oem peek / poke ---------------------------------------------------- */

/* `fastboot oem peek:<addr>[:<len>]`, one INFO line per 16 bytes. */
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

    /* An unaligned MMIO read faults. */
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

    /* On MMIO the readback is often not the value written. */
    n = str_append(line, n, sizeof(line), "wrote ");
    n += hex_format(line + n, value, 8);
    n = str_append(line, n, sizeof(line), " -> ");
    n += hex_format(line + n, addr, 8);
    n = str_append(line, n, sizeof(line), " readback ");
    n += hex_format(line + n, read32(addr), 8);
    line[n] = 0;
    fb_result(line);
}

/* ---- oem partition ------------------------------------------------------ */

#define SECTOR_SIZE         512
/* The SDM2 table starts at byte 32, 16 bytes per entry. */
#define PT_SECTORS          4
#define PT_MAX_ENTRIES      ((PT_SECTORS * SECTOR_SIZE - 32) / 16)

/*
 * The hardware answer is EXT_CSD[226] BOOT_SIZE_MULT x 128 KiB, which is not
 * readable from here, so this is the observed 4 MiB. Boot0 and boot1 are
 * always the same size as each other.
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

    n = str_append(line, n, sizeof(line), name);
    while (n < 11)
        line[n++] = ' ';
    n += hex_format(line + n, start, 8);
    line[n++] = ' ';
    n += hex_format(line + n, count, 8);
    line[n++] = ' ';
    n = str_append(line, n, sizeof(line), type);
    line[n] = 0;
    fb_info(line);
}

/* Column header. Padding targets match pt_line's layout: name at 0, start at
 * 11, count at 20, type at 29. */
static void pt_header(void)
{
    char line[FB_RESPONSE_MAX];
    u32 n = 0;

    n = str_append(line, n, sizeof(line), "device");
    while (n < 11)
        line[n++] = ' ';
    n = str_append(line, n, sizeof(line), "start");
    while (n < 20)
        line[n++] = ' ';
    n = str_append(line, n, sizeof(line), "count");
    while (n < 29)
        line[n++] = ' ';
    n = str_append(line, n, sizeof(line), "type");
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
 * Armed streaming source for `upload`. Nothing is buffered; the arming
 * commands only record what to send, so size is not bounded by memory.
 */
enum upload_kind {
    UPLOAD_NONE = 0,
    UPLOAD_MMC,         /* stream off the card   */
    UPLOAD_MEM,         /* stream out of memory  */
    UPLOAD_CETUS,       /* stream CP memory over the monitor link */
    UPLOAD_CETUS_NOR,   /* stream CP NOR, a window at a time      */
};

static struct {
    int kind;
    u32 dev;            /* MMC: eMMC PARTITION_ACCESS */
    u32 start;          /* MMC: absolute start sector */
    u32 sectors;        /* MMC: how many to send      */
    int decrypt;        /* MMC: unwrap the partition AES-XTS on the way out */
    u64 addr;           /* MEM/CETUS: source address  */
    u32 len;            /* MEM/CETUS: byte count      */
} upload_src;

#define DUMP_CHUNK_SECTORS  (64 * 1024 / SECTOR_SIZE)   /* 64 KiB per read */

/* One monitor command per chunk, so a stalled read cannot wedge the whole
 * transfer and USB keeps getting serviced between them. */
#define CETUS_NOR_CHUNK     0x1000u
#define CETUS_DUMP_CHUNK    4096

#define CETUS_FLASH_SIZE    0x4000000u

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
static int resolve_part(const char *name, u32 *dev, u32 *start, u32 *sectors,
                        int *encrypted)
{
    const char *tail = str_after(name, "nflasha");
    u32 want, i, n_part, extent = 0;
    int rc;

    if (encrypted)
        *encrypted = 0;

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
        /* Only the SDM2 slots carry the partition AES-XTS: the boot hardware
         * partitions use a different key, and bare "nflasha" spans the
         * plaintext partition table as well. */
        if (encrypted)
            *encrypted = 1;
    }
    return 0;
}

/*
 * `oem partition dump <name> [<offset> [<size>]]`, offset/size in hex sectors.
 *
 * An SDM2 slot is decrypted on the way out; everything else is handed back as
 * it is stored.
 */
static void cmd_partition_dump(const char *args)
{
    char name[24], line[FB_RESPONSE_MAX];
    const char *p;
    u32 dev = 0, start = 0, sectors = 0, offset = 0, want, n;
    int rc, encrypted = 0;

    p = parse_token(args, name, sizeof(name));
    if (name[0] == 0) {
        fb_fail("usage: oem partition dump <name> [<off> [<size>]]");
        return;
    }

    rc = resolve_part(name, &dev, &start, &sectors, &encrypted);
    if (rc != 0) {
        n = str_append(line, 0, sizeof(line), "cannot resolve ");
        n = str_append(line, n, sizeof(line), name);
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

    upload_src.kind = UPLOAD_MMC;
    upload_src.dev = dev;
    upload_src.start = start + offset;
    upload_src.sectors = want;
    upload_src.decrypt = encrypted;

    n = str_append(line, 0, sizeof(line), "hint: fastboot get_staged ");
    n = str_append(line, n, sizeof(line), name);
    n = str_append(line, n, sizeof(line), ".bin");
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

    if (upload_src.kind == UPLOAD_NONE) {
        fb_fail("nothing armed (try: oem partition dump <name>)");
        return;
    }

    if (upload_src.kind == UPLOAD_MEM) {
        n = str_append(line, 0, sizeof(line), "DATA");
        n += hex_format(line + n, upload_src.len, 8);
        usb_bulk_send(line, n);

        /* Staged through download_buf: the DWC3's AXI master is not assumed
         * to reach the ROM window. 32-bit loads, since these are Device
         * accesses. */
        while (done < upload_src.len) {
            u32 chunk = upload_src.len - done;
            u32 *out = (u32 *)download_buf;
            u32 i;

            if (chunk > DUMP_CHUNK_SECTORS * SECTOR_SIZE)
                chunk = DUMP_CHUNK_SECTORS * SECTOR_SIZE;

            for (i = 0; i < chunk; i += 4)
                *out++ = read32(upload_src.addr + done + i);

            usb_bulk_send(download_buf, chunk);
            done += chunk;
        }

        fb_okay("");
        return;
    }

    if (upload_src.kind == UPLOAD_CETUS_NOR) {
        n = str_append(line, 0, sizeof(line), "DATA");
        n += hex_format(line + n, upload_src.len, 8);
        usb_bulk_send(line, n);

        while (done < upload_src.len) {
            u32 chunk = upload_src.len - done;

            if (chunk > CETUS_NOR_CHUNK)
                chunk = CETUS_NOR_CHUNK;

            /* Mid data phase; a FAIL can no longer be sent. */
            if (cetus_nor_read((u32)upload_src.addr + done, download_buf,
                               chunk) != 0)
                return;

            usb_bulk_send(download_buf, chunk);
            done += chunk;
        }

        fb_okay("");
        return;
    }

    if (upload_src.kind == UPLOAD_CETUS) {
        n = str_append(line, 0, sizeof(line), "DATA");
        n += hex_format(line + n, upload_src.len, 8);
        usb_bulk_send(line, n);

        while (done < upload_src.len) {
            u32 chunk = upload_src.len - done;

            if (chunk > CETUS_DUMP_CHUNK)
                chunk = CETUS_DUMP_CHUNK;

            {
                /* Mid data phase; a FAIL can no longer be sent. */
                int rc = cetus_read((u32)(upload_src.addr + done),
                                    download_buf, chunk, 4);
                if (rc != 0) {
                    mira_logx("cetus upload stalled at", done);
                    mira_logx("  rc", (u64)(u32)-rc);
                    return;
                }
            }

            usb_bulk_send(download_buf, chunk);
            done += chunk;
        }

        fb_okay("");
        return;
    }

    n = str_append(line, 0, sizeof(line), "DATA");
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

        /* The tweak is the ABSOLUTE sector number, not an offset into the
         * partition. */
        if (upload_src.decrypt) {
            u32 i;

            for (i = 0; i < chunk; i++) {
                u8 *sec = download_buf + i * SECTOR_SIZE;

                if (spacc_xts_sector(sec, sec, upload_src.start + done + i,
                                     0) != SPACC_OK)
                    return;     /* mid data phase; too late to FAIL */
            }
        }

        usb_bulk_send(download_buf, chunk * SECTOR_SIZE);
        done += chunk;
    }

    fb_okay("");
}

/* Travels the same return path as enum mmc_status, so it must not collide. */
#define FLASH_ERR_CRYPTO    (-20)

/*
 * rc, the controller's Error Interrupt Status and the card's R1 -- the only
 * place a write-protect violation appears, so it is named rather than left to
 * be decoded from the hex.
 */
static void flash_fail(const char *what, int rc)
{
    char line[FB_RESPONSE_MAX];
    u32 n = 0;

    if (rc == FLASH_ERR_CRYPTO) {
        fb_fail("crypto engine failed");
        return;
    }

    if (rc == MMC_ERR_RANGE) {
        fb_fail("write would run past the end of the partition");
        return;
    }

    if (rc == MMC_ERR_CARD_STATUS && (mmc_last_r1 & MMC_R1_WP_VIOLATION))
        fb_info("card reports a write protect violation");

    n = str_append(line, n, sizeof(line), what);
    n = str_append(line, n, sizeof(line), " failed rc ");
    n += hex_format(line + n, (u32)rc, 2);
    n = str_append(line, n, sizeof(line), " err ");
    n += hex_format(line + n, mmc_last_error, 4);
    n = str_append(line, n, sizeof(line), " r1 ");
    n += hex_format(line + n, mmc_last_r1, 8);
    line[n] = 0;
    fb_fail(line);
}

/* ---- flash -------------------------------------------------------------- */

/* Keeps any one eMMC command's busy window short without costing round trips. */
#define FLASH_CHUNK_SECTORS  DUMP_CHUNK_SECTORS

/*
 * Sparse image format, as produced by libsparse. The host converts anything
 * over max-download-size into a series of sparse images sent as separate
 * download/flash pairs; each split after the first opens with a DONT_CARE
 * chunk spanning what is already written, which is the only thing that
 * communicates the offset.
 */
#define SPARSE_MAGIC                0xed26ff3au
#define SPARSE_HEADER_SIZE          28
#define SPARSE_CHUNK_HEADER_SIZE    12

#define SPARSE_CHUNK_RAW            0xcac1
#define SPARSE_CHUNK_FILL           0xcac2
#define SPARSE_CHUNK_DONT_CARE      0xcac3
#define SPARSE_CHUNK_CRC32          0xcac4

static u16 le16(const u8 *p)
{
    return (u16)((u32)p[0] | ((u32)p[1] << 8));
}

/* Scratch for expanding FILL chunks; one write granule's worth. */
static u8 fill_buf[FLASH_CHUNK_SECTORS * SECTOR_SIZE] DMA_SECTION;

/*
 * Ciphertext staging: a sparse FILL chunk writes fill_buf repeatedly at
 * different tweaks, so encryption cannot be done in place.
 */
static u8 crypt_buf[FLASH_CHUNK_SECTORS * SECTOR_SIZE] DMA_SECTION;

/* Where the current flash is going, resolved before any data is written. */
static struct {
    u32 dev;
    u32 start;
    u32 sectors;
    int encrypt;        /* wrap in the partition AES-XTS on the way in */
} flash_dst;

/* Skipped sparse chunks do not count. */
static u32 flash_written;

/* Write `n` sectors at sector `off` within the resolved target. */
static int flash_sectors(u32 off, const u8 *src, u32 n)
{
    if (off > flash_dst.sectors || n > flash_dst.sectors - off)
        return MMC_ERR_RANGE;

    flash_written += n;

    while (n) {
        u32 chunk = n > FLASH_CHUNK_SECTORS ? FLASH_CHUNK_SECTORS : n;
        const u8 *out = src;
        int rc;

        /* Tweaks are ABSOLUTE sector numbers, the same ones the dump path
         * uses. */
        if (flash_dst.encrypt) {
            u32 i;

            for (i = 0; i < chunk; i++) {
                if (spacc_xts_sector(crypt_buf + i * SECTOR_SIZE,
                                     src + i * SECTOR_SIZE,
                                     flash_dst.start + off + i, 1) != SPACC_OK)
                    return FLASH_ERR_CRYPTO;
            }
            out = crypt_buf;
        }

        rc = mmc_write_blocks(flash_dst.start + off, out, chunk);
        if (rc != MMC_OK)
            return rc;
        off += chunk;
        n   -= chunk;
        src += chunk * SECTOR_SIZE;
    }
    return MMC_OK;
}

/* Same, from a repeated 32-bit pattern rather than from a buffer. */
static int flash_fill(u32 off, u32 n, u32 word)
{
    u32 *p = (u32 *)fill_buf;
    u32 i;

    for (i = 0; i < sizeof(fill_buf) / 4; i++)
        p[i] = word;

    while (n) {
        u32 chunk = n > FLASH_CHUNK_SECTORS ? FLASH_CHUNK_SECTORS : n;
        int rc = flash_sectors(off, fill_buf, chunk);

        if (rc != MMC_OK)
            return rc;
        off += chunk;
        n   -= chunk;
    }
    return MMC_OK;
}

/*
 * Walk one sparse image, writing it to the resolved target.
 *
 * Two kinds of failure, told apart by `*err`: a malformed image sets it to a
 * message and returns -1, while an eMMC failure leaves it null and returns the
 * driver's status, which the caller reports with rc/err/r1.
 */
static int flash_sparse(const u8 *img, u32 len, const char **err)
{
    u32 hdr_sz, chunk_hdr_sz, blk_sz, total_blks, total_chunks;
    u32 spb, off, sector = 0, i;

    *err = 0;

    if (len < SPARSE_HEADER_SIZE) {
        *err = "sparse image truncated";
        return -1;
    }
    if (le16(img + 4) != 1) {
        *err = "unsupported sparse major version";
        return -1;
    }

    hdr_sz       = le16(img + 8);
    chunk_hdr_sz = le16(img + 10);
    blk_sz       = le32(img + 12);
    total_blks   = le32(img + 16);
    total_chunks = le32(img + 20);

    /* Multiples of 4, not merely large enough: a RAW chunk's payload is read
     * 32 bits at a time out of this buffer, and these are Device accesses. */
    if (hdr_sz < SPARSE_HEADER_SIZE || (hdr_sz & 3) ||
        chunk_hdr_sz < SPARSE_CHUNK_HEADER_SIZE || (chunk_hdr_sz & 3)) {
        *err = "bad sparse header size";
        return -1;
    }
    if (blk_sz == 0 || (blk_sz % SECTOR_SIZE)) {
        *err = "sparse block size is not a multiple of 512";
        return -1;
    }

    spb = blk_sz / SECTOR_SIZE;

    /* Every split carries the whole image's block count, so an oversized
     * image is rejected on the first rather than part way through. */
    if (total_blks > flash_dst.sectors / spb) {
        *err = "image is larger than the partition";
        return -1;
    }

    off = hdr_sz;

    for (i = 0; i < total_chunks; i++) {
        u32 type, chunk_blks, total_sz, data_len, nsec;
        const u8 *data;
        int rc;

        if (off > len || chunk_hdr_sz > len - off) {
            *err = "sparse chunk header truncated";
            return -1;
        }

        type       = le16(img + off);
        chunk_blks = le32(img + off + 4);
        total_sz   = le32(img + off + 8);

        if (total_sz < chunk_hdr_sz || total_sz > len - off) {
            *err = "bad sparse chunk size";
            return -1;
        }

        data     = img + off + chunk_hdr_sz;
        data_len = total_sz - chunk_hdr_sz;

        /* Convert blocks to sectors only after checking it cannot wrap. */
        if (chunk_blks > 0xffffffffu / spb) {
            *err = "sparse chunk too large";
            return -1;
        }
        nsec = chunk_blks * spb;
        if (nsec > 0xffffffffu - sector) {
            *err = "sparse image overruns the address space";
            return -1;
        }

        switch (type) {
        case SPARSE_CHUNK_RAW:
            /* Divide rather than multiply: nsec * SECTOR_SIZE could wrap. */
            if ((data_len % SECTOR_SIZE) || data_len / SECTOR_SIZE != nsec) {
                *err = "raw chunk size disagrees with its block count";
                return -1;
            }
            rc = flash_sectors(sector, data, nsec);
            if (rc != MMC_OK)
                return rc;
            break;

        case SPARSE_CHUNK_FILL:
            if (data_len != 4) {
                *err = "fill chunk payload is not 4 bytes";
                return -1;
            }
            rc = flash_fill(sector, nsec, le32(data));
            if (rc != MMC_OK)
                return rc;
            break;

        case SPARSE_CHUNK_DONT_CARE:
            if (data_len != 0) {
                *err = "skip chunk carries data";
                return -1;
            }
            break;              /* leave those sectors as they are */

        case SPARSE_CHUNK_CRC32:
            break;              /* whole-image checksum; nothing to write */

        default:
            *err = "unknown sparse chunk type";
            return -1;
        }

        sector += nsec;
        off += total_sz;
    }

    return MMC_OK;
}

/*
 * `fastboot flash <name>`. A raw image goes to sector 0 of the partition; a
 * sparse one places itself.
 */
static void cmd_flash(const char *name)
{
    char line[FB_RESPONSE_MAX];
    const char *err = 0;
    u32 dev = 0, start = 0, sectors = 0, nsec, n;
    int rc, encrypted = 0;

    if (download_len == 0) {
        fb_fail("nothing staged; download first");
        return;
    }

    if (!mmc_ready)
        mmc_init();
    if (!mmc_ready) {
        fb_fail("eMMC not ready (try: oem partition)");
        return;
    }

    rc = resolve_part(name, &dev, &start, &sectors, &encrypted);
    if (rc != 0) {
        n = str_append(line, 0, sizeof(line), "cannot resolve ");
        n = str_append(line, n, sizeof(line), name);
        line[n] = 0;
        fb_fail(line);
        return;
    }

    flash_dst.dev     = dev;
    flash_dst.start   = start;
    flash_dst.sectors = sectors;
    flash_dst.encrypt = encrypted;
    flash_written     = 0;

    rc = mmc_select_partition(dev);
    if (rc != MMC_OK) {
        flash_fail("select", rc);
        return;
    }

    /* The magic alone decides: a truncated sparse image must not be written
     * to the card as though it were data. */
    if (download_len >= 4 && le32(download_buf) == SPARSE_MAGIC) {
        rc = flash_sparse(download_buf, download_len, &err);
        if (err) {
            fb_fail(err);
            return;
        }
    } else {
        /* Round up: the download is a byte count, the card only takes
         * sectors. */
        nsec = (download_len + SECTOR_SIZE - 1) / SECTOR_SIZE;
        if (nsec > sectors) {
            n = str_append(line, 0, sizeof(line), "image is ");
            n += dec_format(line + n, nsec);
            n = str_append(line, n, sizeof(line), " sectors, partition holds ");
            n += dec_format(line + n, sectors);
            line[n] = 0;
            fb_fail(line);
            return;
        }

        /* The download buffer is never cleared between commands. */
        for (n = download_len; n < nsec * SECTOR_SIZE; n++)
            download_buf[n] = 0;

        rc = flash_sectors(0, download_buf, nsec);
    }

    if (rc != MMC_OK) {
        flash_fail("write", rc);
        return;
    }

    n = str_append(line, 0, sizeof(line), "wrote ");
    n += dec_format(line + n, flash_written);
    n = str_append(line, n, sizeof(line), " sectors to ");
    n = str_append(line, n, sizeof(line), name);
    line[n] = 0;
    fb_result(line);
}

static void cmd_partition(void)
{
    char name[16];
    char line[FB_RESPONSE_MAX];
    u32 n_part, i, extent = 0, n;
    int rc, init_rc = 0;

    pt_header();

    /* Not in the SDM2 table, which describes the user area only. Each starts
     * at its own sector 0. */
    pt_line("nflashaB0", 0, BOOT_PART_SECTORS, "raw");
    pt_line("nflashaB1", 0, BOOT_PART_SECTORS, "raw");

    /* Startup init may have failed (card asleep, powered late). Retry now
     * rather than reporting a stale failure. */
    if (!mmc_ready)
        init_rc = mmc_init();

    if (!mmc_ready) {
        /* mmc_init_step: 1 CMD0, 2 CMD1, 3 CMD2, 4 CMD3, 5 CMD7,
         * 6 BUS_WIDTH, 7 HS_TIMING. */
        n = str_append(line, 0, sizeof(line), "mmc init rc ");
        n += hex_format(line + n, (u32)init_rc, 2);
        n = str_append(line, n, sizeof(line), " step ");
        n += dec_format(line + n, mmc_init_step);
        n = str_append(line, n, sizeof(line), " err ");
        n += hex_format(line + n, mmc_last_error, 4);
        n = str_append(line, n, sizeof(line), " ocr ");
        n += hex_format(line + n, mmc_ocr, 8);
        line[n] = 0;
        fb_fail(line);
        return;
    }

    /* The SDM2 table is in the user area; boot0 holds the EXBL Information
     * Sector and boot1 is blank. */
    rc = mmc_select_partition(0);
    if (rc == MMC_OK)
        rc = mmc_read_blocks(0, pt_buf, PT_SECTORS);
    dsb();

    if (rc != MMC_OK || pt_buf[0] != '8' || pt_buf[1] != '2' ||
        pt_buf[2] != '4' || pt_buf[3] != '6') {
        n = str_append(line, 0, sizeof(line), "rc ");
        n += hex_format(line + n, (u32)rc, 2);
        n = str_append(line, n, sizeof(line), " err ");
        n += hex_format(line + n, mmc_last_error, 4);
        n = str_append(line, n, sizeof(line), " magic ");
        n += hex_format(line + n, le32(pt_buf), 8);
        line[n] = 0;
        fb_fail(line);
        return;
    }

    n_part = le32(pt_buf + 8);
    if (n_part > PT_MAX_ENTRIES)
        n_part = PT_MAX_ENTRIES;

    /*
     * "nflasha" is the whole user area. Its true size is EXT_CSD[212]
     * SEC_COUNT, which is not readable from here, so what is printed is the
     * highest start+count over valid entries -- a lower bound.
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

        /* SDM_LABEL_VALID. Invalid entries still consume an index: the
         * device name comes from the slot, not the printed position. */
        if (!(flag & 1))
            continue;

        n = str_copy(name, "nflasha", sizeof(name));
        n += dec_format(name + n, i + 1);   /* 1-indexed, as the dump script does */
        name[n] = 0;

        pt_line(name, start, count, pt_typename(type));
    }

    fb_okay("");
}

/* ---- oem exec ----------------------------------------------------------- */

/*
 * SCTLR.I is independent of SCTLR.M, so the I-cache can be live with the MMU
 * off and hold stale lines for an address just downloaded into.
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
 * The callee runs at EL3 on our stack and is trusted to obey AAPCS64.
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
    /* A misaligned branch is a PC alignment fault. */
    if (addr & 3) {
        fb_fail("address must be 4-byte aligned");
        return;
    }

    /* Before jumping, so a callee that never returns looks different from a
     * command that was rejected. */
    n = str_append(line, 0, sizeof(line), "calling ");
    n += hex_format(line + n, addr, 16);
    line[n] = 0;
    fb_info(line);

    sync_icache();

    ret = ((u64 (*)(void))(unsigned long)addr)();

    n = str_append(line, 0, sizeof(line), "returned ");
    n += hex_format(line + n, ret, 16);
    line[n] = 0;
    fb_result(line);
}

/* ---- oem dumpbrom ------------------------------------------------------- */

/* `fastboot oem dumpbrom` -- stage the mask ROM for `get_staged`. */
#define BROM_BASE   0xFFFF0000ull
#define BROM_SIZE   0xC000

static void cmd_dumpbrom(void)
{
    upload_src.kind = UPLOAD_MEM;
    upload_src.addr = BROM_BASE;
    upload_src.len  = BROM_SIZE;

    fb_result("hint: fastboot get_staged brom.bin");
}

/* ---- oem darwin --------------------------------------------------------- */

static void darwin_show_bytes(const char *what, const u8 *b, u32 n)
{
    char line[FB_RESPONSE_MAX];
    u32 i, k = 0;

    k = str_append(line, k, sizeof(line), what);
    for (i = 0; i < n; i++) {
        line[k++] = ' ';
        k += hex_format(line + k, b[i], 2);
    }
    line[k] = 0;
    fb_info(line);
}

/* `oem darwin` -- report the watchdog without touching it. See
 * darwin_vwdt_state() for how to read the two rows. */
static void cmd_darwin_state(void)
{
    u8 counters[4], reloads[3];

    if (darwin_vwdt_state(counters, reloads) != 0) {
        fb_fail("no reply from darwin (link down?)");
        return;
    }

    darwin_show_bytes("counters @200026fc:", counters, 4);
    darwin_show_bytes("reloads  @2000003c:", reloads, 3);

    fb_okay("");
}

/* `oem darwin peek:<addr>[:<len>]`, over command 0x16. Darwin does not bounds
 * check reads, so this reaches its flash as well as its RAM. */
static void cmd_darwin_peek(const char *args)
{
    const char *p;
    u32 addr, len, off;
    u8 buf[0x78];

    addr = (u32)hex_parse(args, &p);
    if (p == args) {
        fb_fail("usage: oem darwin peek:<addr>[:<len>]");
        return;
    }
    len = 16;
    if (*p == ':')
        len = (u32)hex_parse(p + 1, 0);
    if (len == 0 || len > sizeof(buf)) {
        fb_fail("bad length (max 0x78)");
        return;
    }

    if (darwin_read(addr, buf, len) != 0) {
        fb_fail("no reply from darwin (link down?)");
        return;
    }

    for (off = 0; off < len; off += 8) {
        char line[FB_RESPONSE_MAX];
        u32 i, k = 0;

        k += hex_format(line + k, addr + off, 8);
        line[k++] = ':';
        for (i = 0; i < 8 && off + i < len; i++) {
            line[k++] = ' ';
            k += hex_format(line + k, buf[off + i], 2);
        }
        line[k] = 0;
        fb_info(line);
    }
    fb_okay("");
}

static void cmd_darwin(const char *args)
{
    const char *rest;

    if (str_eq(args, "")) {
        cmd_darwin_state();
        return;
    }
    if ((rest = str_after(args, " peek:")) != 0) {
        cmd_darwin_peek(rest);
        return;
    }
    fb_fail("usage: oem darwin [peek:<addr>[:<len>]]");
}

/* ---- oem cetus ---------------------------------------------------------- */

static u8 cetus_buf[0x200];

static void cetus_fail(const char *what, int rc)
{
    char line[FB_RESPONSE_MAX];
    u32 n = 0;

    if (rc == CETUS_E_RDY) {
        fb_fail("no reply from the CP (try: oem cetus reset)");
        return;
    }

    n = str_append(line, n, sizeof(line), what);
    if (CETUS_IS_STATUS(rc)) {
        n = str_append(line, n, sizeof(line), ": target status ");
        n += hex_format(line + n, CETUS_STATUS(rc), 2);
    } else {
        n = str_append(line, n, sizeof(line), ": rc ");
        n += dec_format(line + n, (u32)(-rc));
    }
    line[n] = 0;
    fb_fail(line);
}

static void cetus_report_link(void)
{
    char line[FB_RESPONSE_MAX];
    u32 n = 0;

    n = str_append(line, n, sizeof(line), "reset ");
    n += hex_format(line + n, cetus_reset_state(), 8);
    n = str_append(line, n, sizeof(line), "  bootmode ");
    n += hex_format(line + n, cetus_bootmode_state(), 8);
    line[n] = 0;
    fb_info(line);
}

static void cetus_report_payload(void)
{
    char line[FB_RESPONSE_MAX];
    u32 id, n = 0, i;

    if (cetus_payload_state() != 0) {
        fb_info("payload not running (ROM monitor only)");
        return;
    }
    if (cetus_nor_id(&id) != 0) {
        fb_info("payload running, flash not answering");
        return;
    }

    /* In arrival order -- manufacturer, type, density -- as a datasheet lists
     * them; the assembled word reads reversed. */
    n = str_append(line, n, sizeof(line), "payload running, flash ");
    for (i = 0; i < 3; i++) {
        n += hex_format(line + n, (id >> (8 * i)) & 0xFF, 2);
        line[n++] = ' ';
    }
    line[n] = 0;
    fb_info(line);
}

static void cmd_cetus_state(void)
{
    int rc;

    cetus_spi_init();
    cetus_report_link();

    rc = cetus_status();
    if (rc != 0) {
        cetus_fail("status check failed", rc);
        return;
    }
    cetus_report_payload();
    fb_okay("");
}

/* Reset the CP and put the bundled payload back on it, as init does. */
static void cmd_cetus_reset(void)
{
    int rc = cetus_bring_up();

    cetus_report_link();

    if (rc != 0) {
        cetus_fail("bring-up failed", rc);
        return;
    }
    cetus_report_payload();
    fb_okay("");
}

static void cetus_dump(u32 addr, const u8 *buf, u32 len, u32 width)
{
    u32 per_line = (width == 1) ? 8 : 16;
    u32 off;

    for (off = 0; off < len; off += per_line) {
        char line[FB_RESPONSE_MAX];
        u32 n = 0, i;

        n += hex_format(line + n, addr + off, 8);
        line[n++] = ':';

        for (i = 0; i < per_line && off + i < len; i += width) {
            u32 v = 0, k;

            for (k = 0; k < width; k++)
                v |= (u32)buf[off + i + k] << (8 * k);
            line[n++] = ' ';
            n += hex_format(line + n, v, (int)(width * 2));
        }
        line[n] = 0;
        fb_info(line);
    }
}

static int cetus_parse_width(const char *p, u32 *width)
{
    if (*p != ':')
        return 0;
    *width = (u32)hex_parse(p + 1, 0);
    return (*width == 1 || *width == 2 || *width == 4) ? 0 : -1;
}

static void cmd_cetus_peek(const char *args)
{
    const char *p;
    u32 addr, len = 16, width = 4;
    int rc;

    addr = (u32)hex_parse(args, &p);
    if (p == args) {
        fb_fail("usage: oem cetus peek:<addr>[:<len>[:<width>]]");
        return;
    }
    if (*p == ':') {
        len = (u32)hex_parse(p + 1, &p);
        if (cetus_parse_width(p, &width) != 0) {
            fb_fail("width must be 1, 2 or 4");
            return;
        }
    }
    if (len == 0 || len > sizeof(cetus_buf)) {
        fb_fail("bad length (max 0x200)");
        return;
    }
    if (len % width || addr % width) {
        fb_fail("address and length must be width-aligned");
        return;
    }

    rc = cetus_read(addr, cetus_buf, len, width);
    if (rc != 0) {
        cetus_fail("read failed", rc);
        return;
    }

    cetus_dump(addr, cetus_buf, len, width);
    fb_okay("");
}

static void cmd_cetus_poke(const char *args)
{
    const char *p;
    u32 addr, value, width = 4, i;
    u8 val[4], back[4];
    char line[FB_RESPONSE_MAX];
    u32 n = 0;
    int rc;

    addr = (u32)hex_parse(args, &p);
    if (*p != ':') {
        fb_fail("usage: oem cetus poke:<addr>:<val>[:<width>]");
        return;
    }
    value = (u32)hex_parse(p + 1, &p);
    if (cetus_parse_width(p, &width) != 0) {
        fb_fail("width must be 1, 2 or 4");
        return;
    }
    if (addr % width) {
        fb_fail("address must be width-aligned");
        return;
    }

    for (i = 0; i < width; i++)
        val[i] = (u8)(value >> (8 * i));

    rc = cetus_write(addr, val, width, width);
    if (rc != 0) {
        cetus_fail("write failed", rc);
        return;
    }

    n = str_append(line, n, sizeof(line), "wrote ");
    n += hex_format(line + n, value, (int)(width * 2));
    n = str_append(line, n, sizeof(line), " -> ");
    n += hex_format(line + n, addr, 8);

    if (cetus_read(addr, back, width, width) == 0) {
        u32 v = 0;

        for (i = 0; i < width; i++)
            v |= (u32)back[i] << (8 * i);
        n = str_append(line, n, sizeof(line), " readback ");
        n += hex_format(line + n, v, (int)(width * 2));
    }
    line[n] = 0;
    fb_result(line);
}

static void cmd_cetus_load(const char *args)
{
    const char *p;
    u32 addr = (u32)hex_parse(args, &p);
    u32 len;
    char line[FB_RESPONSE_MAX];
    u32 n = 0;
    int rc;

    if (p == args) {
        fb_fail("usage: oem cetus load:<addr>");
        return;
    }
    if (addr & 3) {
        fb_fail("address must be 4-byte aligned");
        return;
    }
    if (download_len == 0) {
        fb_fail("nothing staged; download first");
        return;
    }

    len = (download_len + 3) & ~3u;
    while (download_len < len)
        download_buf[download_len++] = 0;

    rc = cetus_write(addr, download_buf, len, 4);
    if (rc != 0) {
        cetus_fail("load failed", rc);
        return;
    }

    n = str_append(line, n, sizeof(line), "loaded ");
    n += hex_format_min(line + n, len);
    n = str_append(line, n, sizeof(line), " bytes -> ");
    n += hex_format(line + n, addr, 8);
    line[n] = 0;
    fb_result(line);
}

/*
 * `oem cetus dumpbrom` -- stage the CP's mask ROM for `get_staged`. The same
 * window as the AP's own.
 */
#define CETUS_BROM_BASE 0xFFFF0000u
#define CETUS_BROM_SIZE 0xC000u

static void cmd_cetus_dumpbrom(void)
{
    int rc = cetus_status();

    if (rc != 0) {
        cetus_fail("status check failed", rc);
        return;
    }

    mira_log("dumpbrom armed");
    upload_src.kind = UPLOAD_CETUS;
    upload_src.addr = CETUS_BROM_BASE;
    upload_src.len  = CETUS_BROM_SIZE;

    fb_result("hint: fastboot get_staged cetus_brom.bin");
}

static void cmd_cetus_rate(const char *args)
{
    const char *p = args;
    u32 sel, cpsdvsr = 2, scr = 0, n = 0;
    char line[FB_RESPONSE_MAX];

    sel = (u32)hex_parse(p, &p);
    if (*p == ':') {
        cpsdvsr = (u32)hex_parse(p + 1, &p);
        if (*p == ':')
            scr = (u32)hex_parse(p + 1, 0);
    }

    if (cetus_set_rate(sel, cpsdvsr, scr) != 0) {
        fb_fail("usage: oem cetus rate:<sel 0-2>:<cpsdvsr even>:<scr>");
        return;
    }

    n = str_append(line, n, sizeof(line), "sck ");
    n += dec_format(line + n, cetus_rate_khz());
    n = str_append(line, n, sizeof(line), " kHz");
    line[n] = 0;
    fb_result(line);
}

/*
 * `oem cetus norcmd:<op>[:<len>[:<dummy>[:<addr>]]]` -- one flash command,
 * printing what comes back. RDID and SFDP both need this.
 */
static void cmd_cetus_norcmd(const char *args)
{
    const char *p = args;
    u32 op, len = 1, dummy = 0, addr = 0, use_addr = 0, i, n = 0;
    u8 buf[8];
    char line[FB_RESPONSE_MAX];
    int rc;

    op = (u32)hex_parse(p, &p);
    if (*p == ':') {
        len = (u32)hex_parse(p + 1, &p);
        if (*p == ':') {
            dummy = (u32)hex_parse(p + 1, &p);
            if (*p == ':') {
                addr = (u32)hex_parse(p + 1, 0);
                use_addr = 1;
            }
        }
    }

    if (len == 0 || len > sizeof(buf)) {
        fb_fail("length must be 1..8");
        return;
    }

    rc = cetus_nor_cmd((u8)op, (u8)dummy, (u8)len, addr, (int)use_addr, buf);
    if (rc != 0) {
        cetus_fail("flash command failed", rc);
        return;
    }

    for (i = 0; i < len; i++) {
        n += hex_format(line + n, buf[i], 2);
        line[n++] = ' ';
    }
    line[n] = 0;
    fb_result(line);
}

/*
 * `oem cetus dumpnor[:<off>[:<len>]]` -- stage the CP's raw NOR. Needs the
 * bundled payload up; the contents are not decrypted.
 */
static void cmd_cetus_dumpnor(const char *args)
{
    const char *p = args;
    u32 off = 0, len = CETUS_FLASH_SIZE;
    char line[FB_RESPONSE_MAX];
    u32 n = 0;
    int rc;

    if (*p == ':') {
        off = (u32)hex_parse(p + 1, &p);
        len = CETUS_FLASH_SIZE - off;
        if (*p == ':')
            len = (u32)hex_parse(p + 1, 0);
    }

    if (off >= CETUS_FLASH_SIZE || len == 0 ||
        len > CETUS_FLASH_SIZE - off || (off | len) & 3) {
        fb_fail("bad range (64 MiB flash, 4-byte aligned)");
        return;
    }

    /* A failure during the upload itself can only truncate the transfer. */
    rc = cetus_nor_read(off, cetus_buf, 0x40);
    if (rc != 0) {
        cetus_fail("no NOR read (see: oem cetus)", rc);
        return;
    }

    upload_src.kind = UPLOAD_CETUS_NOR;
    upload_src.addr = off;
    upload_src.len  = len;

    n = str_append(line, n, sizeof(line), "staged ");
    n += hex_format_min(line + n, len);
    n = str_append(line, n, sizeof(line), " bytes; get_staged cetus_nor.bin");
    line[n] = 0;
    fb_result(line);
}

static void cmd_cetus_exec(const char *args)
{
    const char *p;
    u32 addr;
    char line[FB_RESPONSE_MAX];
    u32 n;
    int rc;

    addr = (u32)hex_parse(args, &p);
    if (p == args) {
        fb_fail("usage: oem cetus exec:<addr>");
        return;
    }
    if (addr & 3) {
        fb_fail("address must be 4-byte aligned");
        return;
    }

    n = str_append(line, 0, sizeof(line), "entering ");
    n += hex_format(line + n, addr, 8);
    line[n] = 0;
    fb_info(line);

    rc = cetus_entry(addr);
    if (rc != 0) {
        cetus_fail("entry failed", rc);
        return;
    }
    fb_result("entered");
}

static void cmd_cetus(const char *args)
{
    const char *rest;

    if (str_eq(args, "")) {
        cmd_cetus_state();
        return;
    }
    if (str_eq(args, " reset")) {
        cmd_cetus_reset();
        return;
    }
    if ((rest = str_after(args, " peek:")) != 0) {
        cmd_cetus_peek(rest);
        return;
    }
    if ((rest = str_after(args, " poke:")) != 0) {
        cmd_cetus_poke(rest);
        return;
    }
    if ((rest = str_after(args, " exec:")) != 0) {
        cmd_cetus_exec(rest);
        return;
    }
    if (str_eq(args, " dumpbrom")) {
        cmd_cetus_dumpbrom();
        return;
    }
    if ((rest = str_after(args, " load:")) != 0) {
        cmd_cetus_load(rest);
        return;
    }
    if ((rest = str_after(args, " rate:")) != 0) {
        cmd_cetus_rate(rest);
        return;
    }
    if ((rest = str_after(args, " norcmd:")) != 0) {
        cmd_cetus_norcmd(rest);
        return;
    }
    if ((rest = str_after(args, " dumpnor")) != 0) {
        cmd_cetus_dumpnor(rest);
        return;
    }
    fb_fail("usage: oem cetus [reset|peek:|poke:|exec:|load:|dumpbrom|dumpnor|norcmd:]");
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
    fb_info("dumpbrom             stage the bootrom for get_staged");
    fb_info("darwin               show the Virtual WDT state");
    fb_info("darwin peek:<addr>[:<len>]   read Darwin memory");
    fb_info("cetus                show the CP monitor link state");
    fb_info("cetus reset          reset the CP and restart its payload");
    fb_info("cetus peek:<addr>[:<len>[:<width>]]");
    fb_info("cetus poke:<addr>:<val>[:<width>]");
    fb_info("cetus exec:<addr>    hand the CP over to addr");
    fb_info("cetus norcmd:<op>[:<len>[:<dummy>[:<addr>]]]");
    fb_info("cetus rate:<sel>:<cpsdvsr>[:<scr>]   set link SCK");
    fb_info("cetus dumpbrom       stage the CP bootrom for get_staged");
    fb_info("cetus load:<addr>    write the staged download to CP memory");
    fb_info("cetus dumpnor[:<off>[:<len>]]   stage the CP NOR");

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
    if ((rest = str_after(args, "partition dump ")) != 0) {
        cmd_partition_dump(rest);
        return;
    }
    if (str_eq(args, "partition")) {
        cmd_partition();
        return;
    }
    if (str_eq(args, "dumpbrom")) {
        cmd_dumpbrom();
        return;
    }
    if ((rest = str_after(args, "darwin")) != 0) {
        cmd_darwin(rest);
        return;
    }
    if ((rest = str_after(args, "cetus")) != 0) {
        cmd_cetus(rest);
        return;
    }
    fb_fail("unknown oem command (try: oem help)");
}

/* ---- getvar ------------------------------------------------------------- */

/*
 * `getvar partition-size:<name>`, in bytes. The host asks before every flash;
 * without an answer it warns about a zero-sized partition each time.
 */
static void cmd_getvar_partition_size(const char *name)
{
    char line[FB_RESPONSE_MAX];
    u32 dev, start, sectors, n;

    if (resolve_part(name, &dev, &start, &sectors, 0) != 0) {
        /* OKAY with an empty value, as for any unknown variable. */
        fb_okay("");
        return;
    }

    n = str_append(line, 0, sizeof(line), "0x");
    n += hex_format_min(line + n, (u64)sectors * SECTOR_SIZE);
    line[n] = 0;
    fb_okay(line);
}

static void cmd_getvar(const char *name)
{
    char line[FB_RESPONSE_MAX];
    const char *rest;
    u32 n;

    if ((rest = str_after(name, "partition-size:")) != 0) {
        cmd_getvar_partition_size(rest);
    } else if (str_eq(name, "version")) {
        fb_okay("0.5");
    } else if (str_eq(name, "product")) {
        fb_okay("ILCE-7M4");
    } else if (str_eq(name, "serialno")) {
        fb_okay("ILCE7M4-fastboot");
    } else if (str_eq(name, "max-download-size")) {
        n = str_append(line, 0, sizeof(line), "0x");
        n += hex_format(line + n, DOWNLOAD_MAX, 8);
        line[n] = 0;
        fb_okay(line);
    } else if (str_eq(name, "downloadsize")) {
        n = str_append(line, 0, sizeof(line), "0x");
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
    n = str_append(line, 0, sizeof(line), "DATA");
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

/* ---- reboot ------------------------------------------------------------- */

/*
 * fb_okay() returns only once the IN transfer completes, so the reply is on
 * the wire. Dropping the D+ pullup before the reset makes the host see a clean
 * unplug rather than a device vanishing mid-bus.
 */
static void cmd_reboot(void)
{
    fb_okay("");

    mdelay(50);
    dwc3_disconnect();
    mdelay(10);

    system_reset();
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
    } else if ((rest = str_after(cmd, "flash:")) != 0) {
        cmd_flash(rest);
    } else if (str_eq(cmd, "upload")) {
        cmd_upload();
    } else if (str_eq(cmd, "reboot")) {
        cmd_reboot();
    } else if (str_after(cmd, "reboot-") != 0) {
        /* reboot-bootloader and friends name a target we cannot deliver.
         * Fail rather than quietly doing a plain reboot. */
        fb_fail("only plain `reboot` is supported");
    } else {
        fb_fail("unknown command");
    }
}

void fastboot_loop(void)
{
    for (;;) {
        u32 n;

        /* Deferred from SET_CONFIGURATION; safe to do here. */
        usb_bulk_enable_pending();

        /* A host that stopped reading mid-reply is recoverable. Anything else
         * keeps pumping events rather than stopping, since a loop that does
         * not takes ep0 down with it. */
        if (usb_bulk_error && usb_bulk_recover() != 0) {
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
