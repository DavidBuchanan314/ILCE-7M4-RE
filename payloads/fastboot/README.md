# fastboot

Vibeslopped bare-metal fastboot payload for ILCE-7M4. Works over the USB 2.0 "multi" port.

So far it supports the following custom oem commands:

```
$ fastboot oem help
(bootloader) help                 list oem commands
(bootloader) peek:<addr>[:<len>]  dump memory, max len 0x1000
(bootloader) poke:<addr>:<val>    32-bit write, reports readback
(bootloader) exec:<addr>          call addr, reports return value
(bootloader) download buffer fe031000 len 00080000
OKAY [  0.000s]
Finished. Total time: 0.000s
```

Support for dumping/flashing partitions (incl boot0/1), or whole flash, with or without the AES-XTS encryption handled transparently.