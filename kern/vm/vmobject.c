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

// Creates a VM_Object.
struct vm_object *
vmo_create (size_t npages)
{

	struct vm_object *vmo = NULL;
	unsigned i; int result;

	DEBUG(DB_VM, "VMObject: vmo_create: npages = %d\n", npages);

	vmo = (struct vm_object *)kmalloc(sizeof(struct vm_object));
	if (vmo == NULL) {
		return (NULL);
	}

	vmo -> lpages = array_create();
	if (vmo -> lpages == NULL) {
		kfree(vmo);
		return (NULL);
	}

	// filled in as_define_region
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

// Copies *vmo into **ret.
int
vmo_copy (struct vm_object *vmo, struct addrspace *newas, struct vm_object **ret)
{

	struct vm_object *newvmo = NULL;
	struct lpage *newlp = NULL;
	struct lpage *lp = NULL;
	int j; int result;

	DEBUG(DB_VM, "VMObject: vmo_copy\n");

	newvmo = vmo_create(array_num(vmo -> lpages));
	if (newvmo == NULL) {
		return (ENOMEM);
	}

	newvmo -> base = vmo -> base;
	newvmo -> redzone = vmo -> redzone;

	for (j = 0; (unsigned)j < array_num(vmo -> lpages); j++) {

		lp = array_get(vmo -> lpages, j);
		newlp = array_get(newvmo -> lpages, j);

		KASSERT(newlp == NULL);

		if (lp != NULL) {
			result = lp_copy(lp, &newlp);
			if (result) {
				vmo_destroy(newas, newvmo);
				return (result);
			}
			array_set(newvmo -> lpages, j, newlp);
		}

	}

	*ret = newvmo;
	return (0);

}

// Resizes a VM_Object.
int
vmo_resize (struct addrspace *as, struct vm_object *vmo, int npages)
{

	struct lpage *lp = NULL;
	int i; int spl; int result;

	DEBUG(DB_VM, "VMObject: vmo_resize: npages = %d\n", npages);

	KASSERT(vmo != NULL);
	KASSERT(vmo -> lpages != NULL);

	if ((unsigned)npages < array_num(vmo -> lpages)) {

		spl = splhigh();

		for (i = npages; (unsigned)i < array_num(vmo -> lpages); i++) {
			lp = array_get(vmo -> lpages, i);
			if (lp != NULL) {
				KASSERT(as != NULL);
				mmu_unmap(as, vmo -> base + PAGE_SIZE*i);
				lp_destroy(lp);
			}
		}

		splx(spl);
		result = array_setsize(vmo -> lpages, npages);

	}
	else if ((unsigned)npages > array_num(vmo -> lpages)) {

		int oldsize = array_num(vmo -> lpages);

		result = array_setsize(vmo -> lpages, npages);
		if (result) {
			return (result);
		}
		for (i = oldsize; i < npages; i++) {
			array_set(vmo -> lpages, i, NULL);
		}

	}

	return (0);

}

// Destroys a VM_Object.
void 					
vmo_destroy (struct addrspace *as, struct vm_object *vmo)
{

	int result;

	DEBUG(DB_VM, "VMObject: vmo_destroy\n");

	result = vmo_resize(as, vmo, 0);
	KASSERT(result == 0);
	
	array_destroy(vmo -> lpages);
	kfree(vmo);

}
