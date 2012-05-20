/*

   CoreMap.c
   Ravi Patel & Rohit Ainapure

*/

#include <types.h>
#include <kern/errno.h>
#include <kern/unistd.h>
#include <lib.h>
#include <vm.h>
#include <addrspace.h>
#include <machine/coremap.h>
#include <spl.h>
#include <machine/tlb.h>
#include <array.h>
#include <wchan.h>
#include <thread.h>
#include <current.h>
#include <vfs.h>
#include <vnode.h>
#include <kern/stat.h>
#include <bitmap.h>
#include <synch.h>
#include <uio.h>

struct lock *paging_lock;

struct cm_entry {
	struct lpage *lpage;
	unsigned kernel:1;		// kernel page
	unsigned allocated:1;
	volatile unsigned pinned:1; 	// page is busy
	int tlbindex:7; 		// tlb index
	struct wchan *wchan;
};

static unsigned cm_entries;
static unsigned cm_kernpages;	/* pages allocated to the kernel */
static unsigned cm_userpages;	/* pages allocated to user progs */
static unsigned cm_freepages;
static unsigned cm_basepage;
static unsigned cm_nexttlb;

// ensure at least 8 non-kernel pages available in memory
#define CM_MIN_SLACK		8

#define COREMAP_TO_PADDR(i)	(((paddr_t)PAGE_SIZE)*((i)+cm_basepage))
#define PADDR_TO_COREMAP(page)	(((page)/PAGE_SIZE) - cm_basepage)

static struct cm_entry *coremap;

void
cm_bootstrap (void)
{

	uint32_t i; unsigned npages; unsigned cmsize;
	paddr_t first; paddr_t last;

	DEBUG(DB_VM, "Coremap: cm_bootstrap\n");

	cm_nexttlb = 0;

	ram_getsize(&first, &last);

	// ensure page-aligned
	KASSERT((first & PAGE_FRAME) == first);
	KASSERT((last & PAGE_FRAME) == last);

	npages = (last - first) / PAGE_SIZE;

	cmsize = npages * sizeof(struct cm_entry);
	cmsize = ROUNDUP(cmsize, PAGE_SIZE);
	KASSERT((cmsize & PAGE_FRAME) == cmsize);

	// steal pages
	coremap = (struct cm_entry *) PADDR_TO_KVADDR(first);
	first += cmsize;
	KASSERT(first < last); // cm_entry is too big

	cm_basepage = first / PAGE_SIZE;
	cm_entries = (last / PAGE_SIZE) - cm_basepage;
	cm_kernpages = 0;
	cm_userpages = 0;
	cm_freepages = cm_entries;

	KASSERT(cm_entries + (cmsize / PAGE_SIZE) == npages);

	// initialize coremap entries
	for (i = 0; i < cm_entries; i++) {
		coremap[i].lpage = NULL;
		coremap[i].kernel = 0;
		coremap[i].allocated = 0;
		coremap[i].pinned = 0;
		coremap[i].tlbindex = -1;
	}

}

///// TLB /////

// - returns index of tlb entry to replace
static unsigned 
tlb_replace (void) 
{

	uint32_t slot;

	DEBUG(DB_VM, "Coremap: tlb_replace\n");
	
	KASSERT(curthread -> t_curspl > 0);

	slot = 0;
	slot = (slot + 1) % NUM_TLB;
	return (slot);

}
// - marks tlb entry as invalid
static void
tlb_invalidate (int tlbindex)
{

	uint32_t elo; uint32_t ehi;
	paddr_t pa;
	unsigned cmix;

	DEBUG(DB_VM, "Coremap: tlb_invalidate: tlbindex = %d\n", tlbindex);

	KASSERT(curthread -> t_curspl > 0);

	tlb_read(&ehi, &elo, tlbindex);
	if (elo & TLBLO_VALID) {
		pa = elo & TLBLO_PPAGE;
		cmix = PADDR_TO_COREMAP(pa);
		KASSERT(cmix < cm_entries);
		KASSERT(coremap[cmix].tlbindex == tlbindex);
		coremap[cmix].tlbindex = -1;
	}

	tlb_write(TLBHI_INVALID(tlbindex), TLBLO_INVALID(), tlbindex);

}

// Clears all TLB Entries;
static void
tlb_clear (void)
{

	int i;

	DEBUG(DB_VM, "Coremap: tlb_clear\n");
	
	KASSERT(curthread -> t_curspl > 0);

	for (i = 0; i < NUM_TLB; i++) {
		tlb_invalidate(i);
	}
	cm_nexttlb = 0;

}

// Searches and Invalidates a TLB Entry for a vaddr Translation.
static void
tlb_unmap (vaddr_t va)
{

	int i;
	uint32_t elo = 0; uint32_t ehi = 0;

	DEBUG(DB_VM, "Coremap: tlb_unmap\n");

	KASSERT(curthread -> t_curspl > 0);
	KASSERT(va < MIPS_KSEG0);

	i = tlb_probe(va & PAGE_FRAME, 0);
	if (i < 0) { return; }
	
	tlb_read(&ehi, &elo, i);
	
	KASSERT(elo & TLBLO_VALID); 
	
	tlb_invalidate(i);

}

// Gets TLB Slot for use;
// - may replace an existing one if necessary
static int
mipstlb_getslot (void)
{

	int i;

	DEBUG(DB_VM, "Coremap: mipstlb_getslot\n");

	if (cm_nexttlb < NUM_TLB) {
		return (cm_nexttlb++);
	}

	// evict
	i = tlb_replace();
	tlb_invalidate(i);
	return (i);

}

///// Memory Allocation /////

static uint32_t 
find_page_replace (void)
{

	DEBUG(DB_VM, "Coremap: find_page_replace\n");

	return (0);

}

static void
page_evict (int where)
{

	DEBUG(DB_VM, "Coremap: page_evict: where = %d\n", where);

	(void)where;

}

static int
kernel_maxed (int pagesneeded)
{

	uint32_t npages;

	npages = cm_kernpages + pagesneeded;
	if (npages >= cm_entries - CM_MIN_SLACK) {
		return (1);
	}
	return (0);

}

static int
page_replace (void)
{

	int where;

	DEBUG(DB_VM, "Coremap: page_replace\n");

	KASSERT(curthread -> t_curspl > 0);
	KASSERT(lock_do_i_hold(paging_lock));

	where = find_page_replace();

	KASSERT(coremap[where].pinned == 0);
	KASSERT(coremap[where].kernel == 0);

	if (coremap[where].allocated) {
		KASSERT(coremap[where].lpage != NULL);
		KASSERT(!(curthread -> t_in_interrupt));
		page_evict(where);
	}

	return (where);

}

static void
mark_allocated (int pos, int pin, int iskern)
{

	if (curthread != NULL) {
		DEBUG(DB_VM, "Coremap: mark_allocated: pos = %d, pin = %d, iskern = %d\n",
			pos, pin, iskern);
	}
		
	KASSERT(coremap[pos].pinned == 0);
	KASSERT(coremap[pos].allocated == 0);
	KASSERT(coremap[pos].kernel == 0);
	KASSERT(coremap[pos].lpage == NULL);

	if (pin) { coremap[pos].pinned = 1; }
	coremap[pos].allocated = 1;
	if (iskern) { coremap[pos].kernel = 1; }

	if (iskern) { cm_kernpages++; }
	else { cm_userpages++; }

	cm_freepages--;
	KASSERT(cm_kernpages + cm_userpages + cm_freepages == cm_entries);

}

static paddr_t
allocate_page (struct lpage *lp, int dopin)
{

	int pos; int iskern; int i;

	if (curthread != NULL) {
		DEBUG(DB_VM, "Coremap: allocate_page: dopin = %d\n", dopin);
	}

	iskern = (lp == NULL);

	if (curthread != NULL && !(curthread -> t_in_interrupt)) {
		lock_acquire(paging_lock);
	}

	if (iskern && kernel_maxed(1)) {
		if (curthread != NULL && !(curthread -> t_in_interrupt)) {
			lock_release(paging_lock);
		}
		return (INVALID_PADDR);
	}

	pos = -1;
	if (cm_freepages > 0) {
		for (i = cm_entries-1; i >= 0; i--) {
			if (!coremap[i].pinned && !coremap[i].allocated) {
				KASSERT(coremap[i].kernel == 0);
				KASSERT(coremap[i].lpage == NULL);
				pos = i;
				break;
			}
		}
	}

	if (pos < 0 && !(curthread -> t_in_interrupt)) {
		KASSERT(cm_freepages == 0);
		pos = page_replace();
	}

	if (pos < 0) {
		if (curthread != NULL && !(curthread -> t_in_interrupt)) {
			lock_release(paging_lock);
		}
		return (INVALID_PADDR);
	}

	mark_allocated(pos, dopin, iskern);
	coremap[pos].lpage = lp;

	// ensure free page not in TLB
	KASSERT(coremap[pos].tlbindex < 0);

	if (curthread != NULL && !(curthread -> t_in_interrupt)) {
		lock_release(paging_lock);
	}

	return (COREMAP_TO_PADDR(pos));

}

// Allocates User-Level Page.
paddr_t
cm_allocuserpage (struct lpage *lp)
{

	if (curthread != NULL) {
		DEBUG(DB_VM, "Coremap: cm_allocuserpage\n");
	}

	KASSERT(!(curthread -> t_in_interrupt));
	return (allocate_page(lp, 1));

}

// Copies Page from frompa to topa.
void
cm_copypage (paddr_t frompa, paddr_t topa)
{

	vaddr_t fromva; vaddr_t tova;

	if (curthread != NULL) {
		DEBUG(DB_VM, "Coremap: cm_copypage\n");
	}

	KASSERT(frompa != topa);
	KASSERT(cm_pageispinned(frompa));
	KASSERT(cm_pageispinned(topa));

	fromva = PADDR_TO_KVADDR(frompa);
	tova = PADDR_TO_KVADDR(topa);
	memcpy((char *)tova, (char *)fromva, PAGE_SIZE);

}

void
cm_zero (paddr_t paddr)
{

	vaddr_t va;

	if (curthread != NULL) {
		DEBUG(DB_VM, "Coremap: cm_zero\n");
	}

	KASSERT(cm_pageispinned(paddr));

	va = PADDR_TO_KVADDR(paddr);
	bzero((char *)va, PAGE_SIZE);

}

// Deallocates Page.
void
cm_deallocpage (paddr_t page, int iskern)
{

	uint32_t ppn; int spl;

	if (curthread != NULL) {
		DEBUG(DB_VM, "Coremap: cm_deallocpage: iskern = %d\n", iskern);
	}

	ppn = PADDR_TO_COREMAP(page);	
	
	spl = splhigh();

	KASSERT(ppn < cm_entries);
	KASSERT(coremap[ppn].allocated);

	// flush tlb mapping
	if (coremap[ppn].tlbindex >= 0) {
		tlb_invalidate(coremap[ppn].tlbindex);
		coremap[ppn].tlbindex = -1;
	}

	coremap[ppn].allocated = 0;
	if (coremap[ppn].kernel) {
		KASSERT(coremap[ppn].lpage == NULL);
		KASSERT(iskern);
		cm_kernpages--;
		coremap[ppn].kernel = 0;
	}
	else {
		KASSERT(coremap[ppn].lpage != NULL);
		KASSERT(!iskern);
		cm_userpages--;
	}
	cm_freepages++;

	coremap[ppn].lpage = NULL;

	splx(spl);

}

// Allocates Kernel-Level Page.
vaddr_t 
alloc_kpages (int npages)
{

	paddr_t pa;

	if (curthread != NULL) {
		DEBUG(DB_VM, "Coremap: alloc_kpages: npages = %d\n", npages);
	}

	if (npages > 1) {
		panic("UH OH");
	}
	else {
		pa = allocate_page(NULL, 0);
	}

	if (pa == INVALID_PADDR) {
		return (0);
	}

	return (PADDR_TO_KVADDR(pa));

}

void 
free_kpages (vaddr_t addr)
{

	if (curthread != NULL) {
		DEBUG(DB_VM, "Coremap: free_kpages\n");
	}
	cm_deallocpage(KVADDR_TO_PADDR(addr), 1);

}

// Marks Page as Pinned.
void
cm_pin (paddr_t paddr)
{

	int spl; unsigned index;

	if (curthread != NULL) {
		DEBUG(DB_VM, "Coremap: cm_pin\n");
	}
	
	index = PADDR_TO_COREMAP(paddr);
	KASSERT(index < cm_entries);

	spl = splhigh();
	if (coremap[index].wchan == NULL) {
		coremap[index].wchan = wchan_create("lpage");
	}
	while (coremap[index].pinned) {
		wchan_sleep(coremap[index].wchan);
	}
	coremap[index].pinned = 1;

	splx(spl);

}

// Checks if Page is Pinned.
int
cm_pageispinned (paddr_t paddr)
{

	int spl; int rv;
	unsigned index;

	if (curthread != NULL) {
		DEBUG(DB_VM, "Coremap: cm_pageispinned\n");
	}

	index = PADDR_TO_COREMAP(paddr);
	KASSERT(index < cm_entries);

	spl = splhigh();
	rv = coremap[index].pinned != 0;
	splx(spl);

	return (rv);

}

// Unpins a Page.
void
cm_unpin (paddr_t paddr)
{

	int spl;
	unsigned index;

	if (curthread != NULL) {
		DEBUG(DB_VM, "Coremap: cm_unpin\n");
	}

	index = PADDR_TO_COREMAP(paddr);
	KASSERT(index < cm_entries);

	spl = splhigh();
	KASSERT(coremap[index].pinned);
	coremap[index].pinned = 0;
	wchan_wakeall(coremap[index].wchan);
	splx(spl);

}

///// MMU Control /////

static struct addrspace *lastas = NULL;

// Sets Address Space in MMU.
void
mmu_setas (struct addrspace *as)
{
	
	int spl;

	if (curthread != NULL) {
		DEBUG(DB_VM, "Coremap: mmu_setas\n");
	}

	spl = splhigh();
	if (as != lastas) {
		lastas = as;
		tlb_clear();
	}
	splx(spl);

}

// Removes a Translation from MMU.
void
mmu_unmap (struct addrspace *as, vaddr_t va)
{

	int spl;

	if (curthread != NULL) {
		DEBUG(DB_VM, "Coremap: mmu_unmap\n");
	}

	spl = splhigh();
	if (as == lastas) {
		tlb_unmap(va);
	}
	splx(spl);

}

// Adds a Translation to MMU.
void
mmu_map (struct addrspace *as, vaddr_t va, paddr_t pa, int writable)
{

	int spl; int tlbindex;
	uint32_t ehi; uint32_t elo;
	unsigned cmix;

	if (curthread != NULL) {
		DEBUG(DB_VM, "Coremap: mmu_map: va = %x, writable = %d\n", va, writable);
	}

	KASSERT(as == lastas);

	KASSERT(pa/PAGE_SIZE >= cm_basepage);
	KASSERT(pa/PAGE_SIZE - cm_basepage < cm_entries);
	
	spl = splhigh();

	tlbindex = tlb_probe(va, 0);
	if (tlbindex < 0) {
		tlbindex = mipstlb_getslot();
	}
	KASSERT(tlbindex >= 0 && tlbindex < NUM_TLB);

	cmix = PADDR_TO_COREMAP(pa);
	KASSERT(cmix < cm_entries);
	if (coremap[cmix].tlbindex != tlbindex) {
		KASSERT(coremap[cmix].tlbindex == -1);
		coremap[cmix].tlbindex = tlbindex;
	}

	ehi = va & TLBHI_VPAGE;
	elo = (pa & TLBLO_PPAGE) | TLBLO_VALID;
	if (writable) {
		elo |= TLBLO_DIRTY;
	}

	tlb_write(ehi, elo, tlbindex);

	splx(spl);

}
