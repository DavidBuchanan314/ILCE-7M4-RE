#ifndef FASTBOOT_LOG_H
#define FASTBOOT_LOG_H

#include "io.h"

/*
 * Status from the AP, carried over the CP's UART on the multi connector.
 *
 * The CP has to be up before this says anything, so the LED remains the only
 * channel for the window before that -- it is what reports a payload that
 * never reached the CP bring-up at all.
 */

void log_init(void);

/* One line, prefixed. Silently does nothing while the link is down. */
void mira_log(const char *msg);

/* Line with a hex value appended, for return codes and register reads. */
void mira_logx(const char *msg, u64 value);

#endif /* FASTBOOT_LOG_H */
