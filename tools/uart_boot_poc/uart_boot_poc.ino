/*

This program resets the AP and then sends a small payload over the serial boot
protocol. The payload turns the LED on (the same LED that normally indicates SD activity etc.)

The baud rate is *approximately* 48000, which is a nonstandard baud exactly half way between
the more standard 38400 and 57600
*/

#include <string.h>

#include <hardware/timer.h>

#include <hardware/structs/sio.h>
#include <hardware/structs/timer.h>

#define MULTI_RST 29
#define MULTI_TX 28

#define US_REJECT 200u
#define US_CARRIER 262u
#define US_BIT 21u

#define PREAMBLE_EDGES 60u
#define CARRIER_PULSES_PER_RECORD 36u

#define RESET_PULSE_US 10u
#define RELEASE_HOLD_US 1000u
#define CARRIER_START_MS 150u
#define TRANSMIT_START_MS 400u

#define UNLOCK_WORD 0x2CF25621ul
#define STUB_ADDR 0xFE020000ul

#define RECORD_HEADER 10u
#define MAX_RECORD 256u
#define N_RECORDS 6u

static const unsigned char PAYLOAD[] = {
#embed "payload.bin"
};

static_assert(RECORD_HEADER + sizeof(PAYLOAD) + 4u <= MAX_RECORD, "payload too large for record buffer");
static_assert(RECORD_HEADER + sizeof(PAYLOAD) <= 0x1000u, "payload exceeds the ROM record length limit");

static uint8_t records[N_RECORDS][MAX_RECORD];
static uint16_t record_len[N_RECORDS];

static inline uint32_t now_us(void) {
  return timer_hw->timerawl;
}

static inline void tx(bool level) {
  if (level) {
    sio_hw->gpio_set = 1u << MULTI_TX;
  } else {
    sio_hw->gpio_clr = 1u << MULTI_TX;
  }
}

static uint32_t crc32(const uint8_t *data, uint32_t n) {
  uint32_t c = 0xFFFFFFFFul;
  for (uint32_t i = 0; i < n; i++) {
    c ^= data[i];
    for (int k = 0; k < 8; k++) {
      c = (c >> 1) ^ (0xEDB88320u & -(c & 1u));
    }
  }
  return ~c;
}

static void put_le32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
  p[2] = (uint8_t)(v >> 16);
  p[3] = (uint8_t)(v >> 24);
}

static uint16_t build_record(uint8_t *out, uint16_t type, uint32_t value,
                             const uint8_t *payload, uint16_t payload_len) {
  uint32_t length = RECORD_HEADER + payload_len;
  put_le32(out, length);
  out[4] = (uint8_t)type;
  out[5] = (uint8_t)(type >> 8);
  put_le32(out + 6, value);
  if (payload_len) {
    memcpy(out + RECORD_HEADER, payload, payload_len);
  }
  put_le32(out + length, crc32(out, length));
  return (uint16_t)(length + 4u);
}

static void build_all_records(void) {
  record_len[0] = build_record(records[0], 0x0000, UNLOCK_WORD, NULL, 0);
  record_len[1] = build_record(records[1], 0x0001, STUB_ADDR, PAYLOAD, sizeof(PAYLOAD));
  record_len[2] = build_record(records[2], 0xFFFF, STUB_ADDR, NULL, 0);
  record_len[3] = build_record(records[3], 0x0000, UNLOCK_WORD, NULL, 0);
  record_len[4] = build_record(records[4], 0xFFFF, STUB_ADDR, NULL, 0);
  record_len[5] = build_record(records[5], 0xFFFF, STUB_ADDR, NULL, 0);
}

static void carrier_pulse(void) {
  busy_wait_us_32(US_REJECT);
  tx(true);
  busy_wait_us_32(US_REJECT);
  tx(false);
}

static void send_byte(uint8_t v) {
  tx(false);
  busy_wait_us_32(US_BIT);
  uint8_t x = 0;
  for (uint32_t i = 0; i < 8; i++) {
    bool bit = (v >> i) & 1u;
    x ^= (uint8_t)bit;
    tx(bit);
    busy_wait_us_32(US_BIT);
  }
  tx(!x);
  busy_wait_us_32(US_BIT);
  tx(true);
  busy_wait_us_32(US_BIT);
}

static void send_record(const uint8_t *buf, uint32_t len) {
  busy_wait_us_32(US_CARRIER);
  tx(true);
  bool level = false;
  for (uint32_t i = 1; i < PREAMBLE_EDGES - 1u; i++) {
    busy_wait_us_32(US_CARRIER);
    tx(level);
    level = !level;
  }
  busy_wait_us_32(US_CARRIER);
  for (uint32_t b = 0; b < len; b++) {
    send_byte(buf[b]);
  }
}

static void pulse_reset(void) {
  digitalWrite(MULTI_RST, LOW);
  pinMode(MULTI_RST, OUTPUT);
  busy_wait_us_32(RESET_PULSE_US);
  pinMode(MULTI_RST, INPUT);
}

static void run_once(void) {
  noInterrupts();

  pulse_reset();
  uint32_t t_transmit = now_us() + TRANSMIT_START_MS * 1000u;

  busy_wait_us_32(CARRIER_START_MS * 1000u);
  tx(false);
  pinMode(MULTI_TX, OUTPUT);

  while ((int32_t)(now_us() - t_transmit) < 0) {
    carrier_pulse();
  }

  for (uint32_t r = 0; r < N_RECORDS; r++) {
    tx(false);
    for (uint32_t c = 0; c < CARRIER_PULSES_PER_RECORD; c++) {
      carrier_pulse();
    }
    send_record(records[r], record_len[r]);
  }

  busy_wait_us_32(RELEASE_HOLD_US);
  pinMode(MULTI_TX, INPUT);

  interrupts();
}

void setup() {
  pinMode(MULTI_RST, INPUT);
  pinMode(MULTI_TX, INPUT);

  build_all_records();

  run_once();
}

void loop() {
}
