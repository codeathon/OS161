/*

   LPage.c
   Ravi Patel & Rohit Ainapure

*/

#include <types.h>
#include <kern/errno.h>
#include <lib.h>
#include <spl.h>
#include <synch.h>
#include <thread.h>
#include <machine/coremap.h>
#include <addrspace.h>
#include <vm.h>

// Creates a Logical Page.
struct lpage *
lp_create (void)
{
	
	struct lpage *lp = NULL;

	DEBUG(DB_VM, "LPage: lp_create\n");

	lp = (struct lpage *)kmalloc(sizeof(struct lpage));
	if (lp == NULL) {
		return (NULL);
	}

	lp -> swapaddr = INVALID_SWAPADDR;
	lp -> paddr = INVALID_PADDR;

	lp -> lock = lock_create("lpage");
	if (lp -> lock == NULL) {
		kfree(lp);
		return (NULL);
	}

	return (lp);

}

// Creates a Logical Page and Allocates Swap & RAM.
static int
lp_setup (struct lpage **lpret, paddr_t *paret)
{

	struct lpage *lp = NULL;
	paddr_t pa;
	off_t swa;

	DEBUG(DB_VM, "LPage: lp_setup\n");

	lp = lp_create();
	if (lp == NULL) {
		return (ENOMEM);
	}

	swa = swap_allocate();
	if (swa == INVALID_SWAPADDR) {
		lp_destroy(lp);
		return (ENOSPC);
	}

	lock_acquire(lp -> lock);

	pa = cm_allocuserpage(lp);
	if (pa == INVALID_PADDR) {
		swap_deallocate(swa);
		lock_release(lp -> lock);
		lp_destroy(lp);
		return (ENOSPC);
	}

	lp -> paddr = pa | LPF_DIRTY | LPF_LOCKED;
	lp -> swapaddr = swa;

	KASSERT(cm_pageispinned(pa));

	*lpret = lp;
	*paret = pa;

	return (0);

}

// Copies Logical Page from fromlp to tolp.
int
lp_copy (struct lpage *fromlp, struct lpage **tolp)
{

	struct lpage *newlp = NULL;
	paddr_t frompa; paddr_t topa;
	off_t swapaddr;
	int result;

	DEBUG(DB_VM, "LPage: lp_copy\n");

	lock_acquire(fromlp -> lock);

	frompa = fromlp -> paddr & PAGE_FRAME;
	if (frompa == INVALID_PADDR) {

		swapaddr = fromlp -> swapaddr;
		lock_release(fromlp -> lock);

		frompa = cm_allocuserpage(fromlp);
		if (frompa == INVALID_PADDR) {
			return (ENOMEM);
		}

		KASSERT(cm_pageispinned(frompa));

		lock_acquire(paging_lock);
		swap_pagein(frompa, swapaddr);
		lock_acquire(fromlp -> lock);
		lock_release(paging_lock);

		KASSERT((fromlp -> paddr & PAGE_FRAME) == INVALID_PADDR);

		fromlp -> paddr = frompa | LPF_LOCKED;

	}
	else {
		cm_pin(frompa);
	}

	KASSERT(cm_pageispinned(frompa));

	result = lp_setup(&newlp, &topa);
	if (result) {
		cm_unpin(frompa);
		lock_release(fromlp -> lock);
		return (result);
	}

	KASSERT(cm_pageispinned(topa));
	KASSERT(cm_pageispinned(frompa));
	cm_copypage(frompa, topa);
	cm_unpin(topa);
	cm_unpin(frompa);

	lock_release(fromlp -> lock);
	lock_release(newlp -> lock);

	*tolp = newlp;
	return (0);

}

int
lp_fault (struct lpage *lp, struct addrspace *as, int faulttype, vaddr_t va)
{

	DEBUG(DB_VM, "LPage: lp_fault\n");

	(void)lp;
	(void)as;
	(void)faulttype;
	(void)va;
	
	return (1);
}

void
lp_evict (struct lpage *lp)
{

	DEBUG(DB_VM, "LPage: lp_evict\n");
	
	(void)lp;

}

// Creates a Zero-Filled Logical Page.
int
lp_zero (struct lpage **lpret) {

	struct lpage *lp = NULL;
	paddr_t pa;
	int result;

	DEBUG(DB_VM, "LPage: lp_zero\n");

	result = lp_setup(&lp, &pa);
	if (result) {
		return (result);
	}
	KASSERT(lock_do_i_hold(lp -> lock));
	KASSERT(cm_pageispinned(pa));

	cm_zero(pa);

	KASSERT(cm_pageispinned(pa));
	cm_unpin(pa);
	lock_release(lp -> lock);

	*lpret = lp;
	return (0);

}

// Destroys a Logical Page.
void 					
lp_destroy (struct lpage *lp)
{

	paddr_t pa;
	int spl;

	DEBUG(DB_VM, "LPage: lp_destroy\n");

	KASSERT(lp != NULL);

	lock_acquire(lp -> lock);
	spl = splhigh();

	pa = lp -> paddr & PAGE_FRAME;
	if (pa != INVALID_PADDR) {

		lock_release(lp -> lock);
		cm_pin(pa);
		lock_acquire(lp -> lock);

		if ((lp -> paddr & PAGE_FRAME) == pa) {
			cm_deallocpage(pa, 0);
		}
		else {
			KASSERT((lp -> paddr & PAGE_FRAME) == INVALID_PADDR);
		}

		cm_unpin(pa);

	}

	splx(spl);
	lock_release(lp -> lock);

	if (lp -> swapaddr != INVALID_SWAPADDR) {
		swap_deallocate(lp -> swapaddr);
	}

	kfree(lp);

}
