/*
 * Copyright (c) 2000, 2001, 2002, 2003, 2004, 2005, 2008, 2009
 *	The President and Fellows of Harvard College.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE UNIVERSITY AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE UNIVERSITY OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#ifndef _VM_H_
#define _VM_H_

#include <machine/vm.h>

/* VM system-related definitions. */

// ADDED //

extern struct lock *paging_lock;

struct lpage {
	paddr_t paddr;
	off_t swapaddr;
	struct lock *lock;
};

#define LPF_DIRTY		0x1
#define LPF_LOCKED		0x2
#define LPF_MASK		0x3
#define LP_ISDIRTY (lp)		((lp) -> lp_paddr & LPF_DIRTY)
#define LP_ISLOCKED (lp)	((lp) -> lp_paddr & LPF_LOCKED)
#define LP_SET (am, bit)	((lp) -> lp_paddr |= (bit))
#define LP_CLEAR (am, bit)	((lp) -> lp_paddr &= ~(paddr_t)(bit))

struct lpage *lp_create (void);
int lp_copy (struct lpage *fromlp, struct lpage **tolp);
int lp_fault (struct lpage *lp, struct addrspace *as, int faulttype, vaddr_t va);
void lp_evict (struct lpage *lp);
int lp_zero (struct lpage **lpret);
void lp_destroy (struct lpage *lp);

struct vm_object {
	struct array *lpages;
	vaddr_t base;
	size_t redzone; // disallow other vm_objects
};

struct vm_object *vmo_create (size_t npages);
int vmo_copy (struct vm_object *vmo, struct addrspace *newas, struct vm_object **ret);
int vmo_resize (struct addrspace *as, struct vm_object *vmo, int npages);
void vmo_destroy (struct addrspace *as, struct vm_object *vmo);

#define INVALID_SWAPADDR (0)

void swap_bootstrap (size_t pmemsize);
void swap_shutdown (void);
off_t swap_allocate (void);
void swap_deallocate (off_t diskpage);
void swap_pagein (paddr_t paddr, off_t swapaddr);
void swap_pageout (paddr_t paddr, off_t swapaddr);

// DEFAULT //

/* Fault-type arguments to vm_fault() */
#define VM_FAULT_READ        0    /* A read was attempted */
#define VM_FAULT_WRITE       1    /* A write was attempted */
#define VM_FAULT_READONLY    2    /* A write to a readonly page was attempted*/


/* Initialization function */
size_t vm_bootstrap(void);

/* Fault handling function called by trap code */
int vm_fault(int faulttype, vaddr_t faultaddress);

/* Allocate/free kernel heap pages (called by kmalloc/kfree) */
vaddr_t alloc_kpages(int npages);
void free_kpages(vaddr_t addr);

/* TLB shootdown handling called from interprocessor_interrupt */
void vm_tlbshootdown_all(void);
void vm_tlbshootdown(const struct tlbshootdown *);


#endif /* _VM_H_ */
