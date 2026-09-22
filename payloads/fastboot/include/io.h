#ifndef FASTBOOT_IO_H
#define FASTBOOT_IO_H

typedef unsigned char      u8;
typedef unsigned short     u16;
typedef unsigned int       u32;
typedef unsigned long long u64;
typedef signed int         s32;

#define BIT(n) (1u << (n))

static inline void write32(u64 addr, u32 val)
{
    *(volatile u32 *)addr = val;
}

static inline u32 read32(u64 addr)
{
    return *(volatile u32 *)addr;
}

/*
 * SDHCI registers must be accessed at their natural width: a 32-bit write to
 * Transfer Mode at 0x0C would also issue the Command register at 0x0E.
 */
static inline void write16(u64 addr, u16 val) { *(volatile u16 *)addr = val; }
static inline u16  read16(u64 addr)           { return *(volatile u16 *)addr; }
static inline void write8(u64 addr, u8 val)   { *(volatile u8 *)addr = val; }
static inline u8   read8(u64 addr)            { return *(volatile u8 *)addr; }

static inline void setbits32(u64 addr, u32 mask)
{
    write32(addr, read32(addr) | mask);
}

static inline void clrbits32(u64 addr, u32 mask)
{
    write32(addr, read32(addr) & ~mask);
}

static inline void clrsetbits32(u64 addr, u32 mask, u32 val)
{
    write32(addr, (read32(addr) & ~mask) | val);
}

static inline void dsb(void)  { __asm__ volatile("dsb sy" ::: "memory"); }
static inline void isb(void)  { __asm__ volatile("isb" ::: "memory"); }

/*
 * The MMU is off, so accesses are Device-nGnRnE and a barrier is all the
 * ordering the DWC3's AXI master needs.
 */
static inline void dma_wmb(void) { dsb(); }

/* Provided by delay.c on top of timer0; see timer.h. */
void udelay(u32 usec);
void mdelay(u32 msec);

static inline u64 read_currentel(void)
{
    u64 v;
    __asm__ volatile("mrs %0, CurrentEL" : "=r"(v));
    return v >> 2;
}

#endif /* FASTBOOT_IO_H */
