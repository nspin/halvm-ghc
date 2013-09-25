#define __XEN__
#include "Rts.h"
#include "RtsUtils.h"
#include "sm/OSMem.h"
#include <runtime_reqs.h>
#include <xen/xen.h>
#include <xen/memory.h>
#include "vmm.h"
#include "memory.h"
#include <assert.h>
#include "hypercalls.h"
#include <sys/mman.h>
#include "locks.h"
#include <errno.h>
#include <string.h>

#define PAGE_ALIGN(t1,t2,x) (t1)(((t2)x + (PAGE_SIZE-1)) & (~(PAGE_SIZE-1)))

extern int            _text;
       unsigned long  cur_pages = 0;
       unsigned long  max_pages = 0;

/******************************************************************************/

mfn_t *p2m_map = NULL;

void set_pframe_used(pfn_t pfn)
{
  assert(pfn < cur_pages);
  assert(p2m_map);
  assert(p2m_map[pfn]);
  p2m_map[pfn] = p2m_map[pfn] | PFN_SET_BIT;
}

void set_pframe_unused(pfn_t pfn)
{
  assert(pfn < cur_pages);
  assert(p2m_map);
  assert(p2m_map[pfn]);
  p2m_map[pfn] = p2m_map[pfn] & (~PFN_SET_BIT);
}

mfn_t get_free_frame()
{
  unsigned long i;

  assert(p2m_map);
  for(i = 0; i < cur_pages; i++)
    if(!(p2m_map[i] & PFN_SET_BIT)) {
      mfn_t retval = p2m_map[i];
      p2m_map[i] = p2m_map[i] | PFN_SET_BIT;
      return retval;
    }

  return 0;
}

unsigned long used_frames(void)
{
  unsigned long i, retval;

  for(i = 0, retval = 0; i < cur_pages; i++)
    if(p2m_map[i] & PFN_SET_BIT)
      retval += 1;

  return retval;
}

/******************************************************************************/

static halvm_mutex_t  memory_search_lock;

unsigned long initialize_memory(start_info_t *start_info, void *init_sp)
{
  domid_t self = DOMID_SELF;
  void *free_space_start, *init_alloc_end, *cur;
  uint32_t i, used_frames;

  /* gather some basic information about ourselves */
  p2m_map   = (mfn_t*)start_info->mfn_list;
  max_pages = HYPERCALL_memory_op(XENMEM_maximum_reservation, &self);
  cur_pages = HYPERCALL_memory_op(XENMEM_current_reservation, &self);

  /* sanity checks */
  assert(p2m_map);
  assert((long)cur_pages > 0);
  assert((long)max_pages > 0);

  /* basic setup */
  init_alloc_end = (void*)(((uintptr_t)init_sp + 0x3FFFFF) & (~0x3FFFFF));
  if( ((uintptr_t)init_alloc_end - (uintptr_t)init_sp) < (512 * 1024) ) {
    /* Xen guarantees at least 4MB alignment and 512kB padding after */
    /* the stack. So the above does the alignment, and this if does  */
    /* the edge case.                                                */
    init_alloc_end = (void*)((uintptr_t)init_alloc_end + (4 * 1024 * 1024));
  }
  used_frames = ((uintptr_t)init_alloc_end - (uintptr_t)&_text) >> PAGE_SHIFT;
  for(i = 0; i < used_frames; i++)
    set_pframe_used(i);

  free_space_start = initialize_vmm(start_info, init_sp);
  free_space_start = PAGE_ALIGN(void*,uintptr_t,free_space_start);
  i = ((uintptr_t)free_space_start - (uintptr_t)&_text) >> PAGE_SHIFT;
  for(cur = free_space_start;
      i < used_frames;
      i++, cur = (void*)((uintptr_t)cur + 4096)) {
    set_pframe_unused(i);
    set_pt_entry(cur, 0);
  }

  /* Finally, initialize the lock */
  initMutex(&memory_search_lock);

  return max_pages;
}

/******************************************************************************/

static inline void *advance_page(void *p)
{
  return CANONICALIZE((void*)((uintptr_t)DECANONICALIZE(p) + 4096));
}

static void *run_search_loop(void *start, size_t length)
{
  void *cur = start, *retval = NULL;
  size_t needed_space = length;

  assert(start);
  while(needed_space > 0) {
    pte_t ent = get_pt_entry(cur);

    if(ENTRY_PRESENT(ent) || ENTRY_CLAIMED(ent)) {
      /* nevermind, we can't use anything we've found up until now */
      needed_space = length;
      retval       = NULL;
    } else {
      /* we can start or extend the current run */
      if(!retval) retval = cur;
      needed_space     = needed_space - PAGE_SIZE;
    }

    if(needed_space > 0) {
      cur = advance_page(cur);

      /* check for wraparound, which is bad */
      if( cur < retval ) {
        needed_space = length;
        retval       = NULL;
      }

      /* if we're back where we started from, give up */
      if( cur == start )
        return NULL;
    }
  }

  return retval;
}

static inline void *find_new_addr(void *start_in, size_t length)
{
  static void *glob_search_hint = (void*)0x1000;
  void *p = PAGE_ALIGN(void*,uintptr_t,start_in);

  /* and if they didn't give us any info (or it's junk, see above), */
  /* let's use a reasonable hint about where to start our search.   */
  if(!p) p = glob_search_hint;
  p = run_search_loop(p, length);
  glob_search_hint = PAGE_ALIGN(void*,uintptr_t,((uintptr_t)p + length));
  return p;
}

void *runtime_alloc(void *start, size_t length_in, int prot)
{
  size_t length = PAGE_ALIGN(size_t,size_t,length_in);
  void *dest, *cur, *end;

  halvm_acquire_lock(&memory_search_lock);
  assert(dest = find_new_addr(start, length));
  cur = dest;
  end = (void*)((uintptr_t)dest + length);
  while( (uintptr_t)cur < (uintptr_t)end ) {
    pte_t entry = get_free_frame() << PAGE_SHIFT;

    if(!entry) {
      /* ACK! We're out of memory */
      cur = dest;
      end = cur;

      /* Free anything we've allocated for this request */
      while( (uintptr_t)cur < (uintptr_t)end ) {
        pte_t entry = get_pt_entry(cur);
        set_pframe_unused(entry >> PAGE_SHIFT);
        set_pt_entry(cur, 0);
      }

      /* and return failure */
      halvm_release_lock(&memory_search_lock);
      return NULL;
    } else {
      entry = entry | PG_PRESENT | PG_USER;
      if(prot & PROT_WRITE)
        entry = entry | PG_READWRITE;
      set_pt_entry(cur, entry);
    }

    cur = advance_page(cur);
  }

  /* done! */
  halvm_release_lock(&memory_search_lock);
  memset(dest, 0, length);
  return dest;
}

void *map_frames(mfn_t *frames, size_t num_frames)
{
  void *dest;
  size_t i;

  halvm_acquire_lock(&memory_search_lock);
  assert(dest = find_new_addr(NULL, num_frames * PAGE_SIZE));
  for(i = 0; i < num_frames; i++)
    set_pt_entry((void*)((uintptr_t)dest + (i * PAGE_SIZE)),
                 (frames[i] << PAGE_SHIFT) | STANDARD_RW_PERMS);
  halvm_release_lock(&memory_search_lock);
  return dest;
}

long pin_frame(int level, mfn_t mfn, domid_t dom)
{
  mmuext_op_t op;

  switch(level) {
    case 1: op.cmd = MMUEXT_PIN_L1_TABLE; break;
    case 2: op.cmd = MMUEXT_PIN_L2_TABLE; break;
    case 3: op.cmd = MMUEXT_PIN_L3_TABLE; break;
    case 4: op.cmd = MMUEXT_PIN_L4_TABLE; break;
    default:
      return -EINVAL;
  }
  op.arg1.mfn = mfn;

  return HYPERCALL_mmuext_op(&op, 1, NULL, dom);
}

void *runtime_realloc(void *start, int can_move, size_t oldlen, size_t newlen)
{
  void *retval, *oldcur, *oldend, *newcur, *newend;
  pte_t page_flags, ent;

  if(!start)
    return NULL;

  /* special case, when we're shrinking */
  if(newlen < oldlen) {
    runtime_free((void*)((uintptr_t)start + newlen), oldlen - newlen);
    return start;
  }

  /* weird case, when things are equal */
  if(newlen == oldlen)
    return start;

  /* get the flags that this entry uses */
  page_flags = get_pt_entry(start);
  page_flags = page_flags & (PAGE_SIZE-1);
  if( !ENTRY_PRESENT(page_flags) && !ENTRY_CLAIMED(page_flags) )
    return NULL;

  /* find where to put the new stuff */
  halvm_acquire_lock(&memory_search_lock);
  oldend = (void*)((uintptr_t)start + oldlen);
  newend = (void*)((uintptr_t)start + newlen);
  while(oldend < newend) {
    ent = get_pt_entry(oldend);
    if( ENTRY_CLAIMED(ent) || ENTRY_PRESENT(ent) )
      break;
    oldend = (void*)((uintptr_t)oldend + PAGE_SIZE);
  }

  if(oldend >= newend) {
    /* in this case, we have sufficient room to map the new stuff at the end */
    oldend = (void*)((uintptr_t)start + oldlen);
    while(oldend < newend)
      set_pt_entry(oldend, (get_free_frame() << PAGE_SHIFT) | page_flags);
    return start;
  }

  /* there isn't room at the end. can we move the pointer? */
  if(!can_move) {
    printf("WARNING: Can't realloc without moving, and !can_move\n");
    halvm_release_lock(&memory_search_lock);
    return NULL;
  }

  /* we can, so we need to find a place to put it */
  retval = find_new_addr(start, newlen);
  if(!retval) {
    halvm_release_lock(&memory_search_lock);
    printf("WARNING: No room for mremap()!\n");
    return NULL;
  }

  /* shift the pages over */
  oldcur = start;  oldend = (void*)((uintptr_t)oldcur + oldlen);
  newcur = retval; newend = (void*)((uintptr_t)newcur + newlen);
  while(newcur < newend) {
    if(oldcur < oldend) {
      ent = get_pt_entry(oldcur);
      set_pt_entry(oldcur, 0);
      set_pt_entry(newcur, ENTRY_MADDR(ent) | page_flags);
    } else {
      ent = get_free_frame();
      if(!ent) {
        halvm_release_lock(&memory_search_lock);
        printf("WARNING: Ran out of free frames in mremap()\n");
        return NULL;
      }
      set_pt_entry(newcur, (ent << PAGE_SHIFT) | page_flags);
    }
    oldcur = (void*)((uintptr_t)oldcur + PAGE_SIZE);
    newcur = (void*)((uintptr_t)newcur + PAGE_SIZE);
  }
  halvm_release_lock(&memory_search_lock);

  return retval;
}

void *claim_shared_space(size_t amt)
{
  void *retval;
  size_t i;

  halvm_acquire_lock(&memory_search_lock);
  amt = (amt + (PAGE_SIZE-1)) & ~(PAGE_SIZE-1);
  retval = find_new_addr(NULL, amt);
  for(i = 0; i < (amt / PAGE_SIZE); i++)
    set_pt_entry((void*)((uintptr_t)retval + (i * PAGE_SIZE)), PG_CLAIMED);
  halvm_release_lock(&memory_search_lock);
  return retval;
}

void runtime_free(void *start, size_t length)
{
  void *end = (void*)((uintptr_t)start + length);

  halvm_acquire_lock(&memory_search_lock);
  while(start < end) {
    pte_t pte = get_pt_entry(start);

    if(ENTRY_PRESENT(pte)) {
      mfn_t mfn = pte >> PAGE_SHIFT;
      pfn_t pfn = machine_to_phys_mapping[mfn];
      if(pfn) set_pframe_unused(pfn);
    }
    set_pt_entry(start, 0);
    start = (void*)((uintptr_t)start + PAGE_SIZE);
  }
  halvm_release_lock(&memory_search_lock);
}

int runtime_memprotect(void *addr, size_t length, int prot)
{
  printf("runtime_memprotect(%p, %d, %d)\n", addr, length, prot);
  return 0; // FIXME
}

int runtime_pagesize()
{
  return PAGE_SIZE;
}

/******************************************************************************/

W_ getPageFaults(void);

W_ getPageSize(void)
{
  return runtime_pagesize();
}

W_ getPageFaults(void)
{
  return 0;
}

void *osGetMBlocks(nat n)
{
  size_t padsize = (n + 1) * MBLOCK_SIZE;
  void *allocp, *retval, *extra;

  allocp = runtime_alloc(NULL, padsize, PROT_READWRITE);
  if(!allocp) {
    printf("WARNING: Out of memory. The GHC RTS is about to go nuts.\n");
    return NULL;
  }

  retval = (void*)(((uintptr_t)allocp + (MBLOCK_SIZE-1)) & ~(MBLOCK_SIZE-1));
  /* free the stuff at the beginning and end that we don't need */
  if(allocp == retval) {
    /* we got back an aligned value, so all the extra is at the end */
    extra = (void*)((uintptr_t)allocp + (n * MBLOCK_SIZE));
    runtime_free(extra, MBLOCK_SIZE);
  } else {
    /* if this case fires, we used some of our extra memory to align the */
    /* return value, so this is going to be a little complicated.        */
    size_t extra_head, extra_tail;

    extra = (void*)((uintptr_t)retval + (n * MBLOCK_SIZE));
    extra_head = (uintptr_t)retval - (uintptr_t)allocp;
    extra_tail = ((uintptr_t)allocp + padsize) - (uintptr_t)extra;

    runtime_free(allocp, extra_head);
    runtime_free(extra, extra_tail);
  }

  return retval;
}

void osFreeAllMBlocks(void)
{
  /* ignore this */
}

void osMemInit(void)
{
  /* ignore this */
}

void osFreeMBlocks(char *addr, nat n)
{
  runtime_free(addr, n * MBLOCK_SIZE);
}

void osReleaseFreeMemory(void)
{
  /* ignore this */
}

void setExecutable(void *p, W_ len, rtsBool exec)
{
  void *end = (void*)((uintptr_t)p + len);

  printf("setExecutable(%p, %d, %d)\n", p, len, exec);
  while((uintptr_t)p < (uintptr_t)end) {
    pte_t entry = get_pt_entry(p);

    if(entry & PG_PRESENT)
      set_pt_entry(p, entry & PG_EXECUTABLE);
    p = (void*)((uintptr_t)p + 4096);
  }
}

void system_wmb()
{
#ifdef __x86_64__
  asm volatile ("sfence" : : : "memory");
#else
  asm volatile ("" : : : "memory");
#endif
}

void system_rmb()
{
#ifdef __x86_64__
  asm volatile ("lfence" : : : "memory");
#else
  asm volatile ("lock; addl $0, 0(%%esp)" : : : "memory");
#endif
}

void system_mb()
{
#ifdef __x86_64__
  asm volatile ("mfence" : : : "memory");
#else
  asm volatile ("lock; addl $0, 0(%%esp)" : : : "memory");
#endif
}

