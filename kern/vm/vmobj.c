/*

   VMObject.c
   Ravi Patel & Rohit Ainapure

*/

#include <types.h>
#include <kern/errno.h>
#include <lib.h>
#include <array.h>
#include <spl.h>
#include <machine/coremap.h>
#include <addrspace.h>
#include <vm.h>

struct vm_object *
vmo_create (size_t npages)
{

	struct vm_object *vmo = NULL;
	unsigned i; int result;

	vmo = (struct vm_object *)kmalloc(sizeof(struct vm_object));
	if (vmo == NULL) {
		return (NULL);
	}

	vmo -> lpages = array_create();
	if (vmo -> lpages == NULL) {
		kfree(vmo);
		return (NULL);
	}

	// fill in later
	vmo -> base = 0xdeadbeef;
	vmo -> redzone = 0xdeafbeef;

	// add zerofilled pages
	result = array_setsize(vmo -> lpages, npages);
	if (result) {
		array_destroy(vmo -> lpages);
		kfree(vmo);
		return (NULL);
	}

	for (i = 0; i < npages; i++) {
		array_set(vmo -> lpages, i, NULL);
	}

	return (vmo);

}

int
vmo_copy (struct vm_object *vmo, struct addrspace *newas, struct vm_object **ret)
{

	(void)vmo;
	(void)newas;
	(void)ret;

	return (0);	

}

void 					
vmo_destroy (struct addrspace *as, struct vm_object *vmo)
{

	(void)as;
	(void)vmo;

}
