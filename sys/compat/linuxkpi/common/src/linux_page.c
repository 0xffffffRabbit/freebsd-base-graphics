/*-
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice unmodified, this list of conditions, and the following
 *    disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/malloc.h>
#include <sys/kernel.h>
#include <sys/sysctl.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/rwlock.h>
#include <sys/proc.h>
#include <sys/sched.h>
#include <sys/sf_buf.h>

#include <linux/page.h>
#include <linux/io.h>
#include <linux/slab.h>
#include <linux/kernel.h>


#include <vm/vm.h>
#include <vm/pmap.h>
#include <vm/vm_param.h>
#include <vm/vm_kern.h>
#include <vm/vm_object.h>
#include <vm/vm_page.h>
#include <vm/vm_pageout.h>
#include <vm/vm_pager.h>
#include <vm/vm_phys.h>
#include <vm/vm_radix.h>
#include <vm/vm_reserv.h>
#include <vm/vm_extern.h>



#if defined(__LP64__)
void *
kmap(vm_page_t page)
{

	return ((void *)PHYS_TO_DMAP(VM_PAGE_TO_PHYS(page)));
}

void *
kmap_atomic(vm_page_t page)
{

	sched_pin();
	return kmap(page);
}
	
void
kunmap(vm_page_t page)
{
	/* no work to be done here */
}

void
kunmap_atomic(void *vaddr)
{
	/* no VM work to be done here */
	sched_unpin();
}

#else

static struct sf_buf *
vtosf(caddr_t vaddr)
{
	panic("IMPLEMENT ME!!!");
	return (NULL);
}

static struct sf_buf *
pagetosf(vm_page_t page)
{
	panic("IMPLEMENT ME!!!");
	return (NULL);
}

void *
kmap(vm_page_t page)
{
	struct sf_buf *sf;

	sf = sf_buf_alloc(page, SFB_NOWAIT | SFB_CPUPRIVATE);
	if (sf == NULL) {
		sched_unpin();
		return (-EFAULT);
	}
	return (char *)sf_buf_kva(sf);
}

void *
kmap_atomic(vm_page_t page)
{
	caddr_t vaddr;

	sched_pin();
	if ((vaddr = kmap(page)) == NULL)
		sched_unpin();
	return (vaddr);
}
	
void
kunmap(vm_page_t page)
{
	struct sf_buf *sf;

	sf = pagetosf(page);
	sf_buf_free(sf);
}

void
kunmap_atomic(caddr_t vaddr)
{

	struct sf_buf *sf;

	sf = vtosf(vaddr);
	sf_buf_free(sf);
	sched_unpin();
}
#endif

void
page_cache_release(vm_page_t page)
{
	vm_page_lock(page);
	vm_page_unwire(page, PQ_INACTIVE);
	vm_page_unlock(page);
}

void *
iomap_atomic_prot_pfn(unsigned long pfn, vm_prot_t prot)
{
	sched_pin();
	return (void *)pmap_mapdev_attr(pfn << PAGE_SHIFT,
					PAGE_SIZE, prot);
}

struct io_mapping *
io_mapping_create_wc(vm_paddr_t base, unsigned long size)
{
	struct io_mapping *iomap;

	if ((iomap = kmalloc(sizeof(*iomap), GFP_KERNEL)) == NULL)
		return (NULL);

	/* resource allocation happens when we look up the address on FreeBSD */
	iomap->base = base;
	iomap->size = size;
	return (iomap);
}

void
iounmap_atomic(void *vaddr)
{
	pmap_unmapdev((vm_offset_t)vaddr, PAGE_SIZE);
	sched_unpin();
}

void *
io_mapping_map_wc(struct io_mapping *mapping, unsigned long offset)
{
	resource_size_t phys_addr;

	BUG_ON(offset >= mapping->size);
	phys_addr = mapping->base + offset;

	return ioremap_wc(phys_addr, PAGE_SIZE);
}


void *
io_mapping_map_atomic_wc(struct io_mapping *mapping,
			 unsigned long offset)
{
	vm_paddr_t phys_addr;
	unsigned long pfn;

	BUG_ON(offset >= mapping->size);
	phys_addr = mapping->base + offset;
	pfn = (unsigned long) (phys_addr >> PAGE_SHIFT);
	mapping->prot = PAT_WRITE_COMBINING;
	return iomap_atomic_prot_pfn(pfn, mapping->prot);
}

void
io_mapping_free(struct io_mapping *mapping)
{
	/* assuming the resource is released elsewhere */
	kfree(mapping);
}

int
set_memory_uc(unsigned long addr, int numpages)
{

	return (pmap_change_attr(addr, numpages, PAT_UNCACHED));
}

int
set_pages_uc(vm_page_t page, int numpages)
{
	unsigned long addr = (unsigned long)page_address(page);

	return set_memory_uc(addr, numpages);
}

int
set_memory_wc(unsigned long addr, int numpages)
{

	return (pmap_change_attr(addr, numpages, PAT_WRITE_COMBINING));
}

/* look at actual flags e.g. GFP_KERNEL | GFP_DMA32 | __GFP_ZERO */
vm_page_t
alloc_page(gfp_t flags)
{
	vm_page_t page;
	int tries;
	int req;

	req = VM_ALLOC_ZERO | VM_ALLOC_NOOBJ;
	tries = 0;
retry:
	page = vm_page_alloc_contig(NULL, 0, req, 1, 0, 0xffffffff,
	    PAGE_SIZE, 0, VM_MEMATTR_UNCACHEABLE);
	if (page == NULL) {
		if (tries < 1) {
			if (!vm_page_reclaim_contig(req, 1, 0, 0xffffffff,
			    PAGE_SIZE, 0))
				VM_WAIT;
			tries++;
			goto retry;
		}
		return (NULL);
	}
	if ((flags & __GFP_ZERO) && ((page->flags & PG_ZERO) == 0))
		pmap_zero_page(page);
	return (page);
}

vm_paddr_t
page_to_phys(vm_page_t page)
{
	return (VM_PAGE_TO_PHYS(page));
}

void *
acpi_os_ioremap(vm_paddr_t pa, vm_size_t size)
{
	return ((void *)pmap_mapbios(pa, size));
}

void
unmap_mapping_range(void *obj, loff_t const holebegin, loff_t const holelen, int even_cows)
{
	vm_object_t devobj;
	vm_page_t page;
	int i, page_count;

	devobj = cdev_pager_lookup(obj);
	if (devobj != NULL) {
		page_count = OFF_TO_IDX(holelen);

		VM_OBJECT_WLOCK(devobj);
retry:
		for (i = 0; i < page_count; i++) {
			page = vm_page_lookup(devobj, i);
			if (page == NULL)
				continue;
			if (vm_page_sleep_if_busy(page, "linuxkpi"))
				goto retry;
			cdev_pager_free_page(devobj, page);
		}
		VM_OBJECT_WUNLOCK(devobj);
		vm_object_deallocate(devobj);
	}
}
