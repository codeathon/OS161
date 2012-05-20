/*

   Swap.c
   Ravi Patel & Rohit Ainapure

*/

#include <types.h>
#include <kern/errno.h>
#include <kern/unistd.h>
#include <kern/stat.h>
#include <lib.h>
#include <bitmap.h>
#include <synch.h>
#include <thread.h>
#include <current.h>
#include <vfs.h>
#include <vnode.h>
#include <uio.h>
#include <machine/coremap.h>
#include <spl.h>
#include <vm.h>
#include <addrspace.h>

struct lock *paging_lock;

void
swap_bootstrap (size_t pmemsize)
{

	(void)pmemsize;

}

void
swap_shutdown (void)
{
	
}

off_t
swap_allocate (void)
{

	return (0);

}

void
swap_deallocate (off_t swapaddr)
{

	(void)swapaddr;

}

void
swap_pagein (paddr_t pa, off_t swapaddr)
{

	(void)pa;
	(void)swapaddr;	

}

void
swap_pageout (paddr_t pa, off_t swapaddr)
{
	
	(void)pa;
	(void)swapaddr;

}
