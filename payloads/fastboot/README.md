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
(bootloader) dumpbrom             stage the bootrom for get_staged
(bootloader) darwin               show the Virtual WDT state
(bootloader) darwin peek:<addr>[:<len>]   read Darwin memory
(bootloader) cetus                show the CP monitor link state
(bootloader) cetus reset          reset the CP and restart its payload
(bootloader) cetus peek:<addr>[:<len>[:<width>]]
(bootloader) cetus poke:<addr>:<val>[:<width>]
(bootloader) cetus exec:<addr>    hand the CP over to addr
(bootloader) cetus norcmd:<op>[:<len>[:<dummy>[:<addr>]]]
(bootloader) cetus rate:<sel>:<cpsdvsr>[:<scr>]   set link SCK
(bootloader) cetus dumpbrom       stage the CP bootrom for get_staged
(bootloader) cetus load:<addr>    write the staged download to CP memory
(bootloader) cetus dumpnor[:<off>[:<len>]]   stage the CP NOR
```

note: the bare "darwin" command existed for WDT debugging and could probably be changed/removed.
