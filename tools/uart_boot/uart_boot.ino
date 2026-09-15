/*

See scripts/push_payload.py for usage

See MULTI_* defines for pinout (they go to the Sony multi-port connector).

*/

#include <string.h>

#include <hardware/timer.h>
#include <hardware/sync.h>
#include <hardware/structs/sio.h>
#include <pico/platform.h>

#define MULTI_TX  28
#define MULTI_RX  29
#define MULTI_RST 27

#define US_REJECT   200u
#define US_PREAMBLE 262u
#define US_BIT       21u

#define PREAMBLE_EDGES 60u
#define RESET_PULSE_US 10u
#define STARVE_US      1200000u

#define VERSION "uart_boot 1"

enum {
  CMD_PING    = 0x01,
  CMD_RESET   = 0x02,
  CMD_CARRIER = 0x03,
  CMD_BLOB    = 0x04,
  CMD_BAUD    = 0x05,
  CMD_UART    = 0x06,

  EVT_ACK  = 0x81,
  EVT_ERR  = 0x82,
  EVT_UART = 0x83,
};

#define MAX_RECORD 4100u
#define MAX_PKT    MAX_RECORD
#define UART_CHUNK 512u

static uint8_t stage[MAX_RECORD];
static volatile uint32_t stage_len;
static volatile bool     stage_full;
static volatile bool     carrier_on;
static volatile bool     req_reset;
static volatile bool     starved;

static bool ack_pending;
static bool payload_uart_is_up;

#define __low_jitter(name) __attribute__((noinline)) __not_in_flash_func(name)

static inline __attribute__((always_inline)) void wait_us(uint32_t us) {
  uint32_t t0 = timer_hw->timerawl;
  while ((uint32_t)(timer_hw->timerawl - t0) < us) {
    tight_loop_contents();
  }
}

static inline __attribute__((always_inline)) void tx(bool level) {
  if (level) {
    sio_hw->gpio_set = 1u << MULTI_TX;
  } else {
    sio_hw->gpio_clr = 1u << MULTI_TX;
  }
}

static void pulse_reset(void) {
  sio_hw->gpio_clr    = 1u << MULTI_RST;
  sio_hw->gpio_oe_set = 1u << MULTI_RST;
  wait_us(RESET_PULSE_US);
  sio_hw->gpio_oe_clr = 1u << MULTI_RST;
}

static void __low_jitter(carrier_pulse)(void) {
  wait_us(US_REJECT);
  tx(true);
  wait_us(US_REJECT);
  tx(false);
}

static void __low_jitter(send_byte)(uint8_t v) {
  tx(false);
  wait_us(US_BIT);
  uint8_t x = 0;
  for (uint32_t i = 0; i < 8; i++) {
    bool bit = (v >> i) & 1u;
    x ^= (uint8_t)bit;
    tx(bit);
    wait_us(US_BIT);
  }
  tx(!x);
  wait_us(US_BIT);
  tx(true);
  wait_us(US_BIT);
}

static void __low_jitter(send_blob)(const uint8_t *buf, uint32_t len) {
  wait_us(US_PREAMBLE);
  tx(true);
  bool level = false;
  for (uint32_t i = 1; i < PREAMBLE_EDGES - 1u; i++) {
    wait_us(US_PREAMBLE);
    tx(level);
    level = !level;
  }
  wait_us(US_PREAMBLE);
  for (uint32_t b = 0; b < len; b++) {
    send_byte(buf[b]);
  }
}

static void __low_jitter(run_carrier)(void) {
  uint32_t irq = save_and_disable_interrupts();

  tx(false);
  sio_hw->gpio_oe_set = 1u << MULTI_TX;

  uint32_t idle_since = timer_hw->timerawl;

  while (carrier_on) {
    if (stage_full) {
      __compiler_memory_barrier();
      send_blob(stage, stage_len);
      tx(false);
      stage_full = false;
      idle_since = timer_hw->timerawl;
    } else {
      carrier_pulse();
      if ((uint32_t)(timer_hw->timerawl - idle_since) > STARVE_US) {
        starved     = true;
        carrier_on  = false;
      }
    }
  }

  sio_hw->gpio_oe_clr = 1u << MULTI_TX;
  restore_interrupts(irq);
}

void setup1(void) {
}

void loop1(void) {
  if (req_reset) {
    pulse_reset();
    req_reset = false;
  }

  if (carrier_on) {
    run_carrier();
  }
}

static uint8_t rx_buf[MAX_PKT];
static uint8_t uart_buf[UART_CHUNK];

static uint8_t  rx_hdr_got;
static uint8_t  rx_type;
static uint16_t rx_len;
static uint16_t rx_body_got;
static uint16_t rx_skip;

static uint32_t le32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void send_pkt(uint8_t type, const void *data, uint16_t len) {
  uint8_t hdr[3] = { type, (uint8_t)len, (uint8_t)(len >> 8) };
  Serial.write(hdr, sizeof(hdr));
  if (len) {
    Serial.write((const uint8_t *)data, len);
  }
}

static void send_text(uint8_t type, const char *s) {
  send_pkt(type, s, (uint16_t)strlen(s));
}

static void payload_uart_down(void) {
  if (!payload_uart_is_up) {
    return;
  }
  Serial1.end();
  pinMode(MULTI_TX, INPUT);
  payload_uart_is_up = false;
}

static void handle_pkt(uint8_t type, uint8_t *d, uint16_t len) {
  switch (type) {
  case CMD_PING:
    send_text(EVT_ACK, VERSION);
    break;

  case CMD_RESET:
    req_reset = true;
    send_pkt(EVT_ACK, NULL, 0);
    break;

  case CMD_CARRIER:
    if (len < 1) {
      send_text(EVT_ERR, "short CMD_CARRIER");
      break;
    }
    if (d[0]) {
      payload_uart_down();
      starved     = false;
      stage_full  = false;
      ack_pending = false;
      carrier_on = true;
    } else {
      carrier_on = false;
    }
    send_pkt(EVT_ACK, NULL, 0);
    break;

  case CMD_BLOB:
    if (!carrier_on) {
      send_text(EVT_ERR, "carrier off");
      break;
    }
    if (stage_full || ack_pending) {
      send_text(EVT_ERR, "slot busy");
      break;
    }
    if (len > MAX_RECORD) {
      send_text(EVT_ERR, "record too long");
      break;
    }
    memcpy(stage, d, len);
    stage_len = len;
    __compiler_memory_barrier();
    stage_full  = true;
    ack_pending = true;
    break;

  case CMD_BAUD: {
    if (len < 4) {
      send_text(EVT_ERR, "short CMD_BAUD");
      break;
    }
    uint32_t baud = le32(d);
    if (baud && carrier_on) {
      send_text(EVT_ERR, "carrier on");
      break;
    }
    payload_uart_down();
    if (baud) {
      Serial1.setFIFOSize(1024);
      Serial1.setTX(MULTI_TX);
      Serial1.setRX(MULTI_RX);
      Serial1.begin(baud);
      payload_uart_is_up = true;
    }
    send_pkt(EVT_ACK, NULL, 0);
    break;
  }

  case CMD_UART:
    if (!payload_uart_is_up) {
      send_text(EVT_ERR, "uart not up");
      break;
    }
    Serial1.write(d, len);
    break;

  default:
    send_text(EVT_ERR, "unknown command");
    break;
  }
}

static void poll_host(void) {
  while (Serial.available()) {
    int c = Serial.read();
    if (c < 0) {
      break;
    }

    if (rx_skip) {
      rx_skip--;
      continue;
    }

    if (rx_hdr_got < 3) {
      if (rx_hdr_got == 0) {
        rx_type = (uint8_t)c;
      } else if (rx_hdr_got == 1) {
        rx_len = (uint16_t)c;
      } else {
        rx_len |= (uint16_t)c << 8;
      }
      rx_hdr_got++;
      if (rx_hdr_got == 3) {
        rx_body_got = 0;
        if (rx_len > MAX_PKT) {
          send_text(EVT_ERR, "packet too long");
          rx_skip    = rx_len;
          rx_hdr_got = 0;
        } else if (rx_len == 0) {
          handle_pkt(rx_type, rx_buf, 0);
          rx_hdr_got = 0;
        }
      }
    } else {
      rx_buf[rx_body_got++] = (uint8_t)c;
      if (rx_body_got == rx_len) {
        handle_pkt(rx_type, rx_buf, rx_len);
        rx_hdr_got = 0;
      }
    }
  }
}

void setup(void) {
  pinMode(MULTI_TX,  INPUT);
  pinMode(MULTI_RX,  INPUT);
  pinMode(MULTI_RST, INPUT);

  Serial.begin(115200);
}

void loop(void) {
  poll_host();

  if (starved) {
    starved     = false;
    stage_full  = false;
    ack_pending = false;
    send_text(EVT_ERR, "starved");
  }

  if (ack_pending && !stage_full) {
    ack_pending = false;
    send_pkt(EVT_ACK, NULL, 0);
  }

  if (payload_uart_is_up) {
    int n = Serial1.available();
    if (n > 0) {
      if (n > (int)sizeof(uart_buf)) {
        n = (int)sizeof(uart_buf);
      }
      for (int i = 0; i < n; i++) {
        uart_buf[i] = (uint8_t)Serial1.read();
      }
      send_pkt(EVT_UART, uart_buf, (uint16_t)n);
    }
  }
}
