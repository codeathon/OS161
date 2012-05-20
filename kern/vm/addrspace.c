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

#include <types.h>
#include <kern/errno.h>
#include <kern/limits.h>
#include <lib.h>
#include <uio.h>
#include <thread.h>
#include <current.h>
#include <vnode.h>
#include <vfs.h>
#include <vm.h>
#include <addrspace.h>
#include <kern/unistd.h>
#include <spl.h>
#include <machine/vm.h>
#include <machine/coremap.h>
#include <array.h>

// Creates an Address Space.
struct addrspace *
as_create (void)
{
	
	struct addrspace *as = NULL;

	DEBUG(DB_VM, "Addrspace: as_create\n");

	as = (struct addrspace *)kmalloc(sizeof(struct addrspace));
	if (as == NULL) { return (NULL); }

	as -> as_objects = array_create();
	if (as -> as_objects == NULL) {
		kfree(as);
		return (NULL);
	}

	return (as);

}

// Copies an Address Space.
// - copy all vm_objects to *dstaddr and return as **ret
int
as_copy (struct addrspace *srcaddr, struct addrspace **ret)
{

	struct addrspace *dstaddr = NULL;
	struct vm_object *vmo = NULL;
	struct vm_object *newvmo = NULL;
	unsigned *addindex = NULL;
	int i; int result;

	DEBUG(DB_VM, "Addrspace: as_copy\n");

	dstaddr = as_create();
	if (dstaddr == NULL) {
		return (ENOMEM);
	}

	KASSERT(srcaddr == curthread -> t_addrspace);

	// copy the vm_objects
	for (i = 0; (unsigned)i < array_num(srcaddr -> as_objects); i++) {
	
		vmo = array_get(srcaddr -> as_objects, i);

		result = vmo_copy(vmo, dstaddr, &newvmo);
		if (result) {
			as_destroy(dstaddr);
			return (result);
		}

		result = array_add(dstaddr -> as_objects, newvmo, addindex);
		if (result) {
			vmo_destroy(dstaddr, newvmo);
			as_destroy(dstaddr);
			return (result);
		}

	}

	*ret = dstaddr;
	return (0);

}

// Handles a Fault.
int
as_fault (struct addrspace *as, int faulttype, vaddr_t va)
{

	struct vm_object *faultvmo = NULL;
	struct lpage *lp = NULL;
	vaddr_t bot; vaddr_t top;
	int i; int index; int result;

	DEBUG(DB_VM, "Addrspace: as_fault\n");

	bot = 0;

	// find vm_object
	for (i = 0; (unsigned)i < array_num(as -> as_objects); i++) {
		
		struct vm_object *vmo = NULL;

		vmo = array_get(as -> as_objects, i);
		bot = vmo -> base;
		top = bot + PAGE_SIZE * array_num(vmo -> lpages);
		if (va >= bot && va < top) {
			faultvmo = vmo;
			break;
		}

	}

	if (faultvmo == NULL) {
		return (EFAULT);
	}

	index = (va - bot) / PAGE_SIZE;
	lp = array_get(faultvmo -> lpages, index);

	if (lp == NULL) {
		result = lp_zero(&lp);
		array_set(faultvmo -> lpages, index, lp);
	}
	
	return (lp_fault(lp, as, faulttype, va));

}

// Loads Address Space into MMU.
void
as_activate (struct addrspace *as)
{

	DEBUG(DB_VM, "Addrspace: as_activate\n");

	KASSERT(as == NULL || as == curthread -> t_addrspace);
	mmu_setas(as);

}

/*
 * Set up a segment at virtual address VADDR of size MEMSIZE. The
 * segment in memory extends from VADDR up to (but not including)
 * VADDR+MEMSIZE.
 *
 * The READABLE, WRITEABLE, and EXECUTABLE flags are set if read,
 * write, or execute permission should be set on the segment. At the
 * moment, these are ignored. When you write the VM system, you may
 * want to implement them.
 */
int
as_define_region (struct addrspace *as, vaddr_t vaddr, size_t sz,
		 int readable, int writeable, int executable)
{

	struct vm_object *vmo = NULL;
	vaddr_t check_vaddr;
	unsigned *addindex = NULL;
	int i; int result;

	DEBUG(DB_VM, "Addrspace: as_define_region\n");

	(void)readable;
	(void)writeable;
	(void)executable;

	sz += vaddr & ~(vaddr_t)PAGE_FRAME;
	vaddr &= PAGE_FRAME;
	sz = (sz + PAGE_SIZE - 1) & PAGE_FRAME;

	// base address must be aligned
	KASSERT((vaddr & PAGE_FRAME) == vaddr);

	// redzone must be aligned
	KASSERT((USERSTACKREDZONE & PAGE_FRAME) == USERSTACKREDZONE);

	// redzone must fit
	KASSERT(vaddr >= USERSTACKREDZONE);
	check_vaddr = vaddr - USERSTACKREDZONE;

	sz = ROUNDUP(sz, PAGE_SIZE);

	// check for overlaps
	for (i = 0; (unsigned)i < array_num(as -> as_objects); i++) {

		vaddr_t bot; vaddr_t top;
		
		vmo = array_get(as -> as_objects, i);
		KASSERT(vmo != NULL);
		bot = vmo -> base;
		top = bot + PAGE_SIZE * array_num(vmo -> lpages);

		// check guard band
		KASSERT(bot >= vmo -> redzone);
		bot = bot - vmo -> redzone;

		// overlap
		if (check_vaddr + sz > bot && check_vaddr < top) {
			return (EINVAL);
		}

	}

	// create new vmo
	vmo = vmo_create(sz / PAGE_SIZE);
	if (vmo == NULL) {
		return (ENOMEM);
	}
	vmo -> base = vaddr;
	vmo -> redzone = USERSTACKREDZONE;

	// add new vmo to parent address space
	result = array_add(as -> as_objects, vmo, addindex);
	if (result) {
		vmo_destroy(as, vmo);
		return (result);
	}

	return (0);

}

int
as_prepare_load (struct addrspace *as)
{
	DEBUG(DB_VM, "Addrspace: as_prepare_load\n");
	(void)as;
	return (0);
}

int
as_complete_load (struct addrspace *as)
{
	DEBUG(DB_VM, "Addrspace: as_complete_load\n");
	(void)as;
	return (0);
}

// Defines User-Level Stack.
int
as_define_stack (struct addrspace *as, vaddr_t *stackptr)
{
	
	int result;

	DEBUG(DB_VM, "Addrspace: as_define_stack\n");

	result = as_define_region(as, USERSTACKBASE, USERSTACKSIZE,
				  1, 1, 0);
	if (result) {
		return (result);
	}

	// initial user-level stack pointer
	*stackptr = USERSTACK;
	
	return (0);

}

// Destroys an Address Space.
void
as_destroy (struct addrspace *as)
{
	
	struct vm_object *vmo = NULL;
	int i;

	DEBUG(DB_VM, "Addrspace: as_destroy: size of as = %d\n", array_num(as -> as_objects));

	for (i = 0; (unsigned)i < array_num(as -> as_objects); i++) {
		vmo = array_get(as -> as_objects, i);
		vmo_destroy(as, vmo);
	}

	array_setsize(as -> as_objects, 0);
	array_destroy(as -> as_objects);
	kfree(as);

}
