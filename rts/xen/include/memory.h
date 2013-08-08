#ifndef RTS_XEN_MEMORY_H
#define RTS_XEN_MEMORY_H

#ifndef __XEN__
#define __XEN__
#endif

#include <stdint.h>
#include <sys/types.h>
#include <xen/xen.h>

#define PAGE_SHIFT                12
#define PAGE_SIZE                 (1 << 12)

#ifdef CONFIG_X86_32
typedef uint32_t mfn_t;
typedef uint32_t pfn_t;
typedef uint32_t maddr_t;
#define PFN_SET_BIT               (1 << 31)
#define CPU_LOCAL_MEM_START       0x4000
#define CPU_LOCAL_MEM_END         (1024 * 4096)
#define IN_HYPERVISOR_SPACE(x)    ((uintptr_t)(x) >= HYPERVISOR_VIRT_START)
#define MEMORY_TYPES_DECLARED
#endif

#ifdef CONFIG_X86_PAE
typedef uint32_t mfn_t;
typedef uint32_t pfn_t;
typedef uint64_t maddr_t;
#define PFN_SET_BIT               (1 << 31)
#define CPU_LOCAL_MEM_START       0x4000
#define CPU_LOCAL_MEM_END         (512 * 4096)
#define IN_HYPERVISOR_SPACE(x)    ((uintptr_t)(x) >= HYPERVISOR_VIRT_START)
#define MEMORY_TYPES_DECLARED
#endif

#ifdef CONFIG_X86_64
typedef uint64_t mfn_t;
typedef uint64_t pfn_t;
typedef uint64_t maddr_t;
#define PFN_SET_BIT               (1UL << 63)
#define CPU_LOCAL_MEM_START       0x4000
#define CPU_LOCAL_MEM_END         (512 * 4096)
#define IN_HYPERVISOR_SPACE(x)    (((uintptr_t)(x) >= HYPERVISOR_VIRT_START) &&\
                                   ((uintptr_t)(x) <  HYPERVISOR_VIRT_END))
#define MEMORY_TYPES_DECLARED
#endif

#ifndef MEMORY_TYPES_DECLARED
#error "Need to be compiled with CONFIG_X86_32, 64, or PAE."
#endif

extern mfn_t  *p2m_map;

void           set_pframe_used(pfn_t);
void           set_pframe_unused(pfn_t);
mfn_t          get_free_frame(void);

unsigned long  initialize_memory(start_info_t *, uint32_t, void *);
void           *map_frames(mfn_t *, size_t);

#endif
