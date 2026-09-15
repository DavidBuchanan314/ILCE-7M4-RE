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
 * SDHCI has 8-, 16- and 32-bit registers and several of them must be accessed
 * at their natural width -- writing a 16-bit register as part of a 32-bit
 * access can trigger the neighbouring register's side effects. The Command
 * register at 0x0E is the obvious trap: a 32-bit write to 0x0C would issue the
 * command before Transfer Mode has settled.
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

/* Read-modify-write a field: clear `mask`, then OR in `val`. */
static inline void clrsetbits32(u64 addr, u32 mask, u32 val)
{
    write32(addr, (read32(addr) & ~mask) | val);
}

static inline void dsb(void)  { __asm__ volatile("dsb sy" ::: "memory"); }
static inline void isb(void)  { __asm__ volatile("isb" ::: "memory"); }

/*
 * Barrier to pair with anything the DWC3's AXI master will read. With the MMU
 * off, AArch64 data accesses are Device-nGnRnE, so these are already strongly
 * ordered and uncached and this is a formality -- but main() checks and reports
 * the actual MMU state, and if it turns out to be on we will need real cache
 * maintenance here rather than just a barrier.
 */
static inline void dma_wmb(void) { dsb(); }

/* ---- timing -------------------------------------------------------------
 *
 * Provided by delay.c on top of the SoC timer0 the mask ROM itself uses; see
 * timer.h for the provenance of the register map and the 4 MHz rate.
 */
void udelay(u32 usec);
void mdelay(u32 msec);

static inline u64 read_currentel(void)
{
    u64 v;
    __asm__ volatile("mrs %0, CurrentEL" : "=r"(v));
    return v >> 2;
}

#endif /* FASTBOOT_IO_H */
