# fastboot

Vibeslopped bare-metal fastboot payload for ILCE-7M4. Works over the USB 2.0 "multi" port.

So far it supports the following custom oem commands:

```
$ fastboot oem help
(bootloader) help                 list oem commands
(bootloader) peek:<addr>[:<len>]  dump memory, max len 0x1000
(bootloader) poke:<addr>:<val>    32-bit write, reports readback
(bootloader) exec:<addr>          call addr, reports return value
(bootloader) partition            list eMMC partitions
(bootloader) partition dump <name> [<off> [<sz>]]
(bootloader) darwin               show the Virtual WDT state
(bootloader) darwin peek:<addr>[:<len>]   read Darwin memory
```

note: the bare "darwin" command existed for WDT debugging and could probably be changed/removed.
