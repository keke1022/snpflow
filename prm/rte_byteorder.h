/* 垫片：把 DPDK 的字节序接口映射到标准 endian.h，
 * 使我们能直接复用 DPDK 的 mlx5_prm.h 而不必抄 5700 行定义。 */
#ifndef _PRM_SHIM_BYTEORDER_H
#define _PRM_SHIM_BYTEORDER_H
#include <endian.h>
#include <limits.h>
#include <linux/types.h>   /* __be16/__be32/__be64，避免与我们的 typedef 冲突 */
#include <assert.h>
#include <stdint.h>

typedef uint16_t rte_be16_t;
typedef uint32_t rte_be32_t;
typedef uint64_t rte_be64_t;
typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

#define rte_cpu_to_be_16(x) htobe16(x)
#define rte_cpu_to_be_32(x) htobe32(x)
#define rte_cpu_to_be_64(x) htobe64(x)
#define rte_be_to_cpu_16(x) be16toh(x)
#define rte_be_to_cpu_32(x) be32toh(x)
#define rte_be_to_cpu_64(x) be64toh(x)

#ifndef MLX5_ASSERT
#define MLX5_ASSERT(x) assert(x)
#endif
#ifndef RTE_STD_C11
#define RTE_STD_C11
#endif

/* DPDK 属性宏的最小映射 */
#ifndef __rte_aligned
#define __rte_aligned(a) __attribute__((__aligned__(a)))
#endif
#ifndef __rte_packed_begin
#define __rte_packed_begin __attribute__((__packed__))
#endif
#ifndef __rte_packed_end
#define __rte_packed_end
#endif
#ifndef __rte_packed
#define __rte_packed __attribute__((__packed__))
#endif
#ifndef RTE_CACHE_LINE_SIZE
#define RTE_CACHE_LINE_SIZE 64
#endif
#ifndef __rte_cache_aligned
#define __rte_cache_aligned __rte_aligned(RTE_CACHE_LINE_SIZE)
#endif
#ifndef __rte_always_inline
#define __rte_always_inline inline __attribute__((always_inline))
#endif
#ifndef RTE_BIT32
#define RTE_BIT32(n) (UINT32_C(1) << (n))
#endif
#ifndef RTE_BIT64
#define RTE_BIT64(n) (UINT64_C(1) << (n))
#endif

#endif /* _PRM_SHIM_BYTEORDER_H */
