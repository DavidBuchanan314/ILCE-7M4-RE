# ILCE-7M4-RE
RE notes for ILCE-7M4, aka Sony A7 IV mirrorless camera

## Memory Map (Main AP)

Linux `/proc/iomem`:
```
dffe0000-dffeffff : MEM
  dffe0000-dffeffff : 0000:00:00.0
f1000000-f1000fff : /soc/serial@f1000000
  f1000000-f1000fff : /soc/serial@f1000000
f1001000-f1001fff : /soc/serial@f1001000
  f1001000-f1001fff : /soc/serial@f1001000
f1002000-f1002fff : /soc/serial@f1002000
  f1002000-f1002fff : /soc/serial@f1002000
f1003000-f1003fff : /soc/serial@f1003000
  f1003000-f1003fff : /soc/serial@f1003000
f1004000-f1004fff : /soc/serial@f1004000
  f1004000-f1004fff : /soc/serial@f1004000
f1012000-f1012fff : /soc/i2c0
f1013000-f1013fff : /soc/i2c1
f1017000-f1017fff : /soc/spi0@F1017000
  f1017000-f1017fff : ssp-pl022
f1018000-f1018fff : /soc/spi1@F1018000
  f1018000-f1018fff : ssp-pl022
f1019000-f1019fff : /soc/spi2@F1019000
  f1019000-f1019fff : ssp-pl022
f101a000-f101afff : /soc/spi3@F101A000
  f101a000-f101afff : ssp-pl022
f101b000-f101bfff : /soc/spi4@F101B000
  f101b000-f101bfff : ssp-pl022
f1057000-f1057fff : /soc/mdma@F1057000
f10cc100-f10d0fff : /soc/usbs/usb@0
f11c0000-f11fffff : phy_reg
f7000000-f737ffff : ctrlreg
400000000-4ffffffff : System RAM
  400080000-40067ffff : Kernel code
  4006d0000-400783fff : Kernel data
```

Things not labeled in iomem (see also [drivers/udif/mach-cxd900xx/platform.c](https://github.com/DavidBuchanan314/Sony-ILCE-7M4-Linux/blob/main/linux-kernel/drivers/udif/mach-cxd900xx/platform.c), [drivers/udif/mach-cxd900xx/include/mach/platform.h](https://github.com/DavidBuchanan314/Sony-ILCE-7M4-Linux/blob/main/linux-kernel/drivers/udif/mach-cxd900xx/include/mach/platform.h))
```
start        end          description
0_f10d_6000  0_f10e_0fff  eMMC/MMC
0_f138_8000  0_????_????  SCU / syscon
0_f7fc_0000  0_????_????  SPACC (crypto engine)
0_fe00_0000  0_feff_ffff  eSRAM
0_ffff_0000  0_ffff_bfff  BootROM (key scrambling table is at ffff_b800)
```
