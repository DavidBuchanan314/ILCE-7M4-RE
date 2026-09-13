The main AP (aka Mira aka CXD90057) bootrom is very easy to dump, it can be read straight out of `/dev/mem` once you have a root shell. Reading beyond the end (0xffffc000) will make the camera reboot.

Cetus, the coprocessor (CXD90058) is less obvious how to dump. Fortunately the AP can reboot it into a special SPI monitor boot mode, which has 3 commands: write memory, checksum memory, execute an address. The client side of this protocol can be found in update images in `spicom.elf`. `ud_spicom_boot.elf` reboots it into the SPI boot mode.

Maybe you can do something clever with the execute feature, but I used the checksum command to read out one byte at a time (the checksum algorithm is a trivial 32-bit sum of bytes).

By the way, the AP also has an SPI boot mode, but I can't figure out how to enter it, or where the SPI bus is physically.

The AP also has a UART boot mode (bit-banged over GPIO), which I'm in the process of figuring out.