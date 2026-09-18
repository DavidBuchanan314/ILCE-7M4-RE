#include "io.h"
#include "log.h"
#include "cetus.h"

#define LOG_MAX 64

static int log_ready;

static const char hexdigits[] = "0123456789abcdef";

void log_init(void)
{
    log_ready = (cetus_payload_state() == 0);
}

static u32 append(char *dst, u32 n, u32 size, const char *src)
{
    while (*src && n + 1 < size)
        dst[n++] = *src++;
    return n;
}

void mira_log(const char *msg)
{
    char line[LOG_MAX];
    u32 n;

    if (!log_ready)
        return;

    n = append(line, 0, sizeof(line), "[mira] ");
    n = append(line, n, sizeof(line), msg);
    line[n] = 0;
    cetus_println(line);
}

void mira_logx(const char *msg, u64 value)
{
    char line[LOG_MAX];
    u32 n;
    int i;

    if (!log_ready)
        return;

    n = append(line, 0, sizeof(line), "[mira] ");
    n = append(line, n, sizeof(line), msg);
    n = append(line, n, sizeof(line), " 0x");
    for (i = 7; i >= 0 && n + 1 < sizeof(line); i--)
        line[n++] = hexdigits[(value >> (i * 4)) & 0xF];
    line[n] = 0;
    cetus_println(line);
}
