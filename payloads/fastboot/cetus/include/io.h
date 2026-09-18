#ifndef CETUS_IO_H
#define CETUS_IO_H

typedef unsigned char      u8;
typedef unsigned short     u16;
typedef unsigned int       u32;
typedef unsigned long long u64;

#define BIT(n) (1u << (n))

static inline void write32(u64 addr, u32 val) { *(volatile u32 *)addr = val; }
static inline u32  read32(u64 addr)           { return *(volatile u32 *)addr; }
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

static inline void dsb(void) { __asm__ volatile("dsb sy" ::: "memory"); }
static inline void isb(void) { __asm__ volatile("isb" ::: "memory"); }

#endif /* CETUS_IO_H */
