/*

   CoreMap.h
   Ravi Patel & Rohit Ainapure

*/

#ifndef _MIPS_COREMAP_H_
#define _MIPS_COREMAP_H_

#include <machine/vm.h>

struct addrspace;
struct lpage;

#define INVALID_PADDR ((paddr_t)0)

void 	mmu_setas (struct addrspace *as);
void 	mmu_unmap (struct addrspace *as, vaddr_t va);
void 	mmu_map (struct addrspace *as, vaddr_t va, paddr_t pa, int writable);

void 	cm_bootstrap (void);

paddr_t	cm_allocuserpage (struct lpage *lp);
void 	cm_copypage (paddr_t frompa, paddr_t topa);
void 	cm_zero (paddr_t paddr);
void 	cm_deallocpage (paddr_t page, int iskern);

void 	cm_pin (paddr_t paddr);
int 	cm_pageispinned (paddr_t paddr);
void 	cm_unpin (paddr_t paddr);

#endif
