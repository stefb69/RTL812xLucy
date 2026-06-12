/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * compat/IOKit/IOLib.h -- DriverKit stand-in for the kernel <IOKit/IOLib.h>.
 *
 * The RTL812xLucy hardware layer (rtl812xx.cpp, rtl8127_fiber.cpp) is
 * compiled unchanged inside the dext. Its only kernel dependency is the
 * <IOKit/IOLib.h> include pulled by linux/linux.h; this header satisfies it
 * with DriverKit equivalents plus the handful of libkern helpers the
 * compat layer uses.
 */

#ifndef RTL8127DEXT_COMPAT_IOKIT_IOLIB_H
#define RTL8127DEXT_COMPAT_IOKIT_IOLIB_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <limits.h>

#include <DriverKit/IOLib.h>
#include <DriverKit/IOTypes.h>

/* Legacy Mac type names used throughout the ported sources. */
typedef uint8_t   UInt8;
typedef uint16_t  UInt16;
typedef uint32_t  UInt32;
typedef uint64_t  UInt64;
typedef int8_t    SInt8;
typedef int16_t   SInt16;
typedef int32_t   SInt32;
typedef int64_t   SInt64;

#ifndef OS_INLINE
#define OS_INLINE static inline
#endif

#ifndef TRUE
#define TRUE 1
#endif
#ifndef FALSE
#define FALSE 0
#endif

#ifndef PAGE_SIZE
#if defined(__arm64__) || defined(__aarch64__)
#define PAGE_SIZE 16384
#else
#define PAGE_SIZE 4096
#endif
#endif
#ifndef PAGE_MASK
#define PAGE_MASK (PAGE_SIZE - 1)
#endif

/* errno values used by the ported code (the DriverKit SDK has no errno.h). */
#ifndef EIO
#define EIO         5
#endif
#ifndef EBUSY
#define EBUSY       16
#endif
#ifndef ENODEV
#define ENODEV      19
#endif
#ifndef EINVAL
#define EINVAL      22
#endif
#ifndef ETIMEDOUT
#define ETIMEDOUT   60
#endif
#ifndef EOPNOTSUPP
#define EOPNOTSUPP  102
#endif

#ifndef min
#define min(a, b) ((a) < (b) ? (a) : (b))
#endif
#ifndef max
#define max(a, b) ((a) > (b) ? (a) : (b))
#endif

/* BSD bits referenced by rtl812x_dash.h (declarations only in the dext). */
struct sockaddr {
    UInt8 sa_len;
    UInt8 sa_family;
    char  sa_data[14];
};

#ifndef SIOCDEVPRIVATE
#define SIOCDEVPRIVATE 0x89F0
#endif

/* PCIe constants (kIOPCILinkControlASPMBits*, config offsets, ...). */
#include <PCIDriverKit/IOPCIFamilyDefinitions.h>

#ifndef LONG_BIT
#define LONG_BIT 64
#endif

#ifndef NSEC_PER_USEC
#define NSEC_PER_USEC 1000ULL
#endif
#ifndef NSEC_PER_SEC
#define NSEC_PER_SEC 1000000000ULL
#endif

#ifndef OSSwapInt16
#define OSSwapInt16(x) __builtin_bswap16((uint16_t)(x))
#endif
#ifndef OSSwapInt32
#define OSSwapInt32(x) __builtin_bswap32((uint32_t)(x))
#endif
#ifndef OSSwapInt64
#define OSSwapInt64(x) __builtin_bswap64((uint64_t)(x))
#endif

/* MMIO accessors over the userspace-mapped BAR (little-endian device). */
#ifndef OSWriteLittleInt16
OS_INLINE void OSWriteLittleInt16(volatile void *base, uintptr_t off, uint16_t v)
{
    *(volatile uint16_t *)((uintptr_t)base + off) = OSSwapHostToLittleInt16(v);
}
#endif

#ifndef OSWriteLittleInt32
OS_INLINE void OSWriteLittleInt32(volatile void *base, uintptr_t off, uint32_t v)
{
    *(volatile uint32_t *)((uintptr_t)base + off) = OSSwapHostToLittleInt32(v);
}
#endif

#ifndef OSReadLittleInt16
OS_INLINE uint16_t OSReadLittleInt16(const volatile void *base, uintptr_t off)
{
    return OSSwapLittleToHostInt16(*(volatile const uint16_t *)((uintptr_t)base + off));
}
#endif

#ifndef OSReadLittleInt32
OS_INLINE uint32_t OSReadLittleInt32(const volatile void *base, uintptr_t off)
{
    return OSSwapLittleToHostInt32(*(volatile const uint32_t *)((uintptr_t)base + off));
}
#endif

/* Atomics used by the linux compat helpers. */
OS_INLINE SInt32 OSIncrementAtomic(volatile SInt32 *addr)
{
    return __atomic_fetch_add(addr, 1, __ATOMIC_SEQ_CST);
}

OS_INLINE SInt32 OSDecrementAtomic(volatile SInt32 *addr)
{
    return __atomic_fetch_sub(addr, 1, __ATOMIC_SEQ_CST);
}

OS_INLINE SInt32 OSAddAtomic(SInt32 amount, volatile SInt32 *addr)
{
    return __atomic_fetch_add(addr, amount, __ATOMIC_SEQ_CST);
}

OS_INLINE SInt16 OSAddAtomic16(SInt16 amount, volatile SInt16 *addr)
{
    return __atomic_fetch_add(addr, amount, __ATOMIC_SEQ_CST);
}

OS_INLINE UInt32 OSBitOrAtomic(UInt32 mask, volatile UInt32 *addr)
{
    return __atomic_fetch_or(addr, mask, __ATOMIC_SEQ_CST);
}

OS_INLINE UInt32 OSBitAndAtomic(UInt32 mask, volatile UInt32 *addr)
{
    return __atomic_fetch_and(addr, mask, __ATOMIC_SEQ_CST);
}

/* Opaque lock type; the ported code only declares pointers to it. */
typedef struct RTLCompatSimpleLock IOSimpleLock;

/*
 * Minimal clock shims for linux.h's usleep_range()/fsleep(): treat
 * "absolute time" as nanoseconds and busy-wait with IODelay. Only short
 * waits flow through this path in the ported code.
 */
OS_INLINE void nanoseconds_to_absolutetime(uint64_t ns, uint64_t *result)
{
    *result = ns;
}

OS_INLINE void clock_get_uptime(uint64_t *result)
{
    *result = 0;
}

OS_INLINE void clock_delay_until(uint64_t deadline)
{
    IODelay(deadline / NSEC_PER_USEC);
}

#endif /* RTL8127DEXT_COMPAT_IOKIT_IOLIB_H */
