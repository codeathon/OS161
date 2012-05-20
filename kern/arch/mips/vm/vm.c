/*

   VM.c
   Ravi Patel & Rohit Ainapure

*/

#include <types.h>
#include <kern/errno.h>
#include <lib.h>
#include <synch.h>
#include <vm.h>
#include <machine/coremap.h>
#include <addrspace.h>
#include <thread.h>
#include <current.h>
#include <mainbus.h>

size_t
vm_bootstrap (void)
{

	cm_bootstrap();
	paging_lock = lock_create("paging_lock");
	return (mainbus_ramsize());
	
}

void
vm_tlbshootdown_all (void)
{
	panic("dumbvm tried to do tlb shootdown?!\n");
}

void
vm_tlbshootdown (const struct tlbshootdown *ts)
{
	(void)ts;
	panic("dumbvm tried to do tlb shootdown?!\n");
}

int
vm_fault (int faulttype, vaddr_t faultaddress)
{

	struct addrspace *as = NULL;

	faultaddress &= PAGE_FRAME;
	KASSERT(faultaddress < MIPS_KSEG0);

	as = curthread -> t_addrspace;
	if (as == NULL) {
		return (EFAULT);
	}

	return (as_fault(as, faulttype, faultaddress));

}
