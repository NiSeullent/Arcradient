/*
 * Arcradient (MSHTML-Next)
 * include/arcr_module.h - Versioned Pure C Module ABI & Capability Verification
 */
#ifndef ARCR_MODULE_H
#define ARCR_MODULE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32)
#  define ARCR_API __declspec(dllexport)
#elif defined(__GNUC__) && __GNUC__ >= 4
#  define ARCR_API __attribute__((visibility("default")))
#else
#  define ARCR_API
#endif

#define ARCR_MODULE_MAGIC    0x52435241u /* "ARCR" little-endian */
#define ARCR_ABI_VERSION     0x00010000u /* v1.0 */
#define ARCR_ENGINE_BUILD_ID 0x20261112u

/* OS major/minor encoded as (major << 8) | minor */
#define ARCR_OS_WIN2000      0x0500u
#define ARCR_OS_WINXP        0x0501u
#define ARCR_OS_WINXP_X64    0x0502u
#define ARCR_OS_WIN10        0x0A00u
#define ARCR_OS_POSIX_ANY    0x0001u

typedef enum ArcrArchId {
    ARCR_ARCH_UNKNOWN = 0,
    ARCR_ARCH_X86_32  = 1,
    ARCR_ARCH_X86_64  = 2,
    ARCR_ARCH_ARM64   = 3
} ArcrArchId;

typedef enum ArcrCpuFlags {
    ARCR_CPU_SCALAR = 0u,
    ARCR_CPU_MMX    = 1u << 0,
    ARCR_CPU_SSE1   = 1u << 1,
    ARCR_CPU_SSE2   = 1u << 2,
    ARCR_CPU_SSE3   = 1u << 3,
    ARCR_CPU_SSSE3  = 1u << 4,
    ARCR_CPU_SSE4_1 = 1u << 5,
    ARCR_CPU_SSE4_2 = 1u << 6,
    ARCR_CPU_AVX    = 1u << 7,
    ARCR_CPU_AVX2   = 1u << 8
} ArcrCpuFlags;

typedef enum ArcrModuleStatus {
    ARCR_MOD_OK                    = 0,
    ARCR_MOD_ERR_NULL              = 1,
    ARCR_MOD_ERR_BAD_MAGIC         = 2,
    ARCR_MOD_ERR_BAD_STRUCT_SIZE   = 3,
    ARCR_MOD_ERR_ABI_MISMATCH      = 4,
    ARCR_MOD_ERR_BUILD_MISMATCH    = 5,
    ARCR_MOD_ERR_ARCH_MISMATCH     = 6,
    ARCR_MOD_ERR_OS_TOO_OLD        = 7,
    ARCR_MOD_ERR_CPU_UNSUPPORTED   = 8,
    ARCR_MOD_ERR_MISSING_VTABLE    = 9
} ArcrModuleStatus;

typedef struct ArcrModuleDescriptor {
    uint32_t    magic;              /* Must equal ARCR_MODULE_MAGIC */
    uint32_t    struct_size;        /* Must equal sizeof(ArcrModuleDescriptor) */
    uint32_t    abi_version;        /* Must equal ARCR_ABI_VERSION */
    uint32_t    engine_build_id;    /* Must equal ARCR_ENGINE_BUILD_ID */
    uint16_t    arch_id;            /* ArcrArchId */
    uint16_t    min_os_major_minor; /* e.g., ARCR_OS_WIN2000 (0x0500) */
    uint32_t    required_cpu_flags; /* Bitmask of ArcrCpuFlags */
    const char *module_name;        /* e.g., "arcr-js-portable" */
    const void *vtable;             /* Module-specific function table */
} ArcrModuleDescriptor;

typedef const ArcrModuleDescriptor *(*ArcrModuleQueryFn)(void);

/* Compile-time architecture helper */
static inline ArcrArchId arcr_host_compile_arch(void) {
#if defined(__x86_64__) || defined(_M_X64)
    return ARCR_ARCH_X86_64;
#elif defined(__i386__) || defined(_M_IX86)
    return ARCR_ARCH_X86_32;
#elif defined(__aarch64__) || defined(_M_ARM64)
    return ARCR_ARCH_ARM64;
#else
    return ARCR_ARCH_UNKNOWN;
#endif
}

/* Validates a dynamically loaded module descriptor before invoking any code */
ArcrModuleStatus arcr_module_validate(
    const ArcrModuleDescriptor *desc,
    ArcrArchId                  host_arch,
    uint16_t                    host_os_version,
    uint32_t                    host_cpu_flags
);

const char *arcr_module_status_str(ArcrModuleStatus status);

#ifdef __cplusplus
}
#endif

#endif /* ARCR_MODULE_H */
