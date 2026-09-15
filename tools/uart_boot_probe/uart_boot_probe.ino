/*

This program makes the camera fail to boot, with a "13 short flashes" error code.

This corresponds to error code 0x100D, which indicates a parity error.

This demonstrates that we're reaching the serial bootloader code.
(The error is expected, because we just keep sending garbage after the preamble)

Pro tip: If your camera "dies", don't panic, just pull the battery and it should be ok again.

*/

#define MULTI_RST 27
#define MULTI_TX 28

void setup() {
  pinMode(MULTI_RST, INPUT);
  digitalWrite(MULTI_RST, LOW);
  delay(500);
  pinMode(MULTI_RST, OUTPUT);
  delayMicroseconds(10);
  pinMode(MULTI_RST, INPUT);

  delay(200);

  digitalWrite(MULTI_TX, LOW);
  pinMode(MULTI_TX, OUTPUT);
  for (;;) {
    digitalWrite(MULTI_TX, HIGH);
    delayMicroseconds(233);
    digitalWrite(MULTI_TX, LOW);
    delayMicroseconds(233);
  }
}

void loop() {
}
