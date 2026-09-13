`emmc_boot0.bin` is a raw dump of the camera's eMMC boot0 partition (aka `/dev/nflashaB0`), corresponding to firmware version v2.00. boot1 is all zeroes.

The next-stage bootloader is encrypted. `../scripts/parse_emmc_boot0.py` decrypts it using the same algorithm as the bootloader, to produce the loaded eSRAM image at `esram_0xfe040000.bin`.

The entrypoint for the next stage is `0xfe049000` (entered in aarch64 mode).

`emmc_boot0.bin` also contains some other stuff, aside from the emmc bootloader, which I haven't really looked into yet.
