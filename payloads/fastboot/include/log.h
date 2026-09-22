#ifndef FASTBOOT_LOG_H
#define FASTBOOT_LOG_H

#include "io.h"

/* Status from the AP, carried over the CP's UART on the multi connector. */

void log_init(void);

/* One line, prefixed. Silently does nothing while the link is down. */
void mira_log(const char *msg);

/* Line with a hex value appended. */
void mira_logx(const char *msg, u64 value);

#endif /* FASTBOOT_LOG_H */
