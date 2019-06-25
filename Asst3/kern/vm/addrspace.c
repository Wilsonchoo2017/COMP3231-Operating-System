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
#include <lib.h>
#include <spl.h>
#include <spinlock.h>
#include <current.h>
#include <mips/tlb.h>
#include <addrspace.h>
#include <vm.h>
#include <proc.h>
#include <synch.h>

/*
 * Note! If OPT_DUMBVM is set, as is the case until you start the VM
 * assignment, this file is not compiled or linked or in any way
 * used. The cheesy hack versions in dumbvm.c are used instead.
 *
 * UNSW: If you use ASST3 config as required, then this file forms
 * part of the VM subsystem.
 *
 */

/*
 * allocate a data structure used to keep track of
 * an address space
 */
struct addrspace *
as_create(void)
{

	struct addrspace *as;
	as = kmalloc(sizeof(struct addrspace));
	if (as == NULL) {
		return NULL;
	}

	/*
	 * Initialize as needed.
	 */
	as->head = NULL; /* region initialisation */
	/* PD initialisation */
	paddr_t **pd = kmalloc(PAGETABLE_SIZE * 4);
	if(pd == NULL) {
		kfree(as);
		return NULL;
	}
	as->pagetable = pd;
	for (int i = 0; i < PAGETABLE_SIZE; i++) {
		pd[i] = NULL;
	}
	as->pt_lock = lock_create("lock for page table"); /* pd lock initialisation */

	return as;
}

/*
 * allocates a new (destination) address space
 * adds all the same regions as source
 * roughly, for each mapped page in source
 * - allocate a frame in dest
 * - copy contents from source frame to dest frame
 * - add PT entry for dest
 */
int
as_copy(struct addrspace *old, struct addrspace **ret)
{
	if (old == NULL) {
		return EINVAL;
	}
	struct addrspace *newas;
	newas = as_create();
	if (newas == NULL) {
		return ENOMEM;
	}
	
	/*
	 * get all data from old and copy them into new
	 * Have to allocate memory for anything that we copy
	 * prolly a good idea to lock what we are trying to copy here
	 */
	lock_acquire(old->pt_lock);
	/* copy head region */
	struct region *old_curr = old->head;
	struct region *new_curr = newas->head;

	while(old_curr != NULL)	{
		struct region * new_region = kmalloc(sizeof(struct region));
		if(new_region == NULL) {
			as_destroy(newas);
			lock_release(old->pt_lock);
			return ENOMEM;
		}

		region_copy(old_curr, new_region);
		/* check if we are dealing with starting node */
		if(new_curr != NULL) {
			new_curr->next = new_region;
		} else {
			newas->head = new_region;
		}
		new_curr = new_region;
		old_curr = old_curr->next;
	}




	/* Copy pt into new*/
	 if(pt_copy(old, old->pagetable, newas->pagetable) != 0)
	 {
		as_destroy(newas);
		lock_release(old->pt_lock);
		return ENOMEM;
	 }

	lock_release(old->pt_lock);
	*ret = newas;
	return 0;
}
/*
 * deallocate book keeping and page tables
 * - deallocate the frames used
 */
void
as_destroy(struct addrspace *as)
{
	/*
      * Clean up as needed.
      */
	/* Have to verify that the addrspace isnt used atm */
	lock_acquire(as->pt_lock);
	/* free pagetable in a 2 level table fashion */
	for (int i = 0; i < PAGETABLE_SIZE; i++) {
		if (as->pagetable[i] != NULL) {
			for(int j = 0; j < PAGETABLE_SIZE; j++) {
				if(as->pagetable[i][j] != EMPTY) {
					free_kpages(PADDR_TO_KVADDR(as->pagetable[i][j]) & PAGE_FRAME);
				}
			}
			kfree(as->pagetable[i]);
		}
	}
	kfree(as->pagetable);
	/* free region in a linked list fashio */
	regions_cleanup(as);
	lock_release(as->pt_lock);
	lock_destroy(as->pt_lock);
	kfree(as);
}

/*
 * flush TLB (or set the hardware asid)
 * use what is from dumbvm
 */
void
as_activate(void)
{
	struct addrspace *as;

	as = proc_getas();
	if (as == NULL) {
		/*
		 * Kernel thread without an address space; leave the
		 * prior address space in place.
		 */
		return;
	}

	/*
	 * From dumbvm.c
	 * Disable interrupts on this CPU while frobbing the TLB.
	 * its zeroing out the tlb
	 */
	int spl = splhigh();

	for (int i = 0; i < NUM_TLB; i++) {
		tlb_write(TLBHI_INVALID(i), TLBLO_INVALID(), i);
	}

	splx(spl);
}

/*
 * flush TLB (or flush an asid)
 */
void
as_deactivate(void)
{
	/*
	 * Write this. For many designs it won't need to actually do
	 * anything. See proc.c for an explanation of why it (might)
	 * be needed.
	 * in what circumstances do we need this? Just flush
	 */
    as_activate();
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
as_define_region(struct addrspace *as, vaddr_t vaddr, size_t memsize,
		 int readable, int writeable, int executable)
{
	/*
 	 * Region is per-processs that will define the region of memeroy
	 * for that said process
	 * When defining a region must check
	 * - vaddr + memsize stays within kuseg
	 * - it does not overlap with other regions
	 * -
 	 */

	/* Align the addresses to page boundaries */
    /* Align the region. First, the base... */
    memsize += vaddr & ~(vaddr_t)PAGE_FRAME;
    vaddr &= PAGE_FRAME;

    /* ...and now the length. */
    memsize = (memsize + PAGE_SIZE - 1) & PAGE_FRAME;

	
	int result = region_valid(as, vaddr, memsize);
	if (result) {
		return result; 
	}
	
	struct region *new = region_create(vaddr, memsize, readable, writeable, executable);
	if (new == NULL) return ENOMEM;
	region_insert(as, new);
	return 0;
}

/*
 * make READONLY regions READWRITE for loading
 * purposes
 */
int
as_prepare_load(struct addrspace *as)
{
	struct region *curr = as->head;
	while(curr != NULL) {
		curr->old_writeable = curr->writeable;
		curr->writeable = 1;
		curr = curr->next;
	}

	return 0;
}

/*
 * enforce READONLY again
 */
int
as_complete_load(struct addrspace *as)
{
	as_activate(); //Flush after loading
	lock_acquire(as->pt_lock);
	paddr_t **pt = as->pagetable;
	for(int i = 0; i < PAGETABLE_SIZE; i++) {
		if (pt[i] != NULL) {
			for(int j = 0; j < PAGETABLE_SIZE; j++) {
				if (pt[i][j] != EMPTY) {
					vaddr_t pd_bits = i << 22;
					vaddr_t pt_bits = j << 12;
					vaddr_t vaddr = pd_bits|pt_bits;
					struct region *region = get_region(as, vaddr);
					if (region->old_writeable == 0) {
						pt[i][j] = (pt[i][j] & PAGE_FRAME) | TLBLO_VALID;
					}
				}
			}
		}
	}
	lock_release(as->pt_lock);
	struct region *curr = as->head;
	while(curr != NULL) {
		curr->writeable = curr->old_writeable;
		curr = curr->next;
	}
	
	return 0;
}

int
as_define_stack(struct addrspace *as, vaddr_t *stackptr)
{

	/* Initial user-level stack pointer */
	*stackptr = USERSTACK;

	return as_define_region(as, *stackptr - USER_STACK_SIZE, USER_STACK_SIZE, 1, 1, 0);
}

/*
 * REGION FUNCTIONS
 */
struct region * region_create(vaddr_t vaddr, size_t memsize,
		 int readable, int writeable, int executable) {

	struct region *new = kmalloc(sizeof(struct region));
	if (new != NULL) {
		new->base_addr = vaddr;
		new->memsize = memsize;
		new->readable = readable;
		new->writeable = writeable;
		new->executable = executable;
		new->next = NULL;
	}

	return new;
}

void region_insert(struct addrspace *as, struct region *new) {
	new->next = as->head;
	as->head = new;
}


void region_copy(struct region *old, struct region *new) {
	new->base_addr = old->base_addr;
	new->memsize = old->memsize;
	new->readable = old->readable;
	new->writeable = old->writeable;
	new->old_writeable = old->old_writeable;
	new->executable = old->executable;
	new->next = NULL;
}


/*
 * 1) Check if the region is defined within kuseg
 * 2) Check if the region does not overlap other regions
 */
int region_valid(struct addrspace *as, vaddr_t vaddr, size_t memsize) {
	if ((vaddr + memsize) > MIPS_KSEG0) {
		return EFAULT;
	}

	
	struct region *curr = as->head;
	while(curr != NULL) {
		if (curr->base_addr <= vaddr && 
			(curr->base_addr + curr->memsize) > vaddr) {
			return EINVAL; //TODO Check correct error
		} else if (curr->base_addr <= (vaddr+memsize) && 
			(curr->base_addr + curr->memsize) > (vaddr+memsize)) {
			return EINVAL;
		} else if (vaddr <= curr->base_addr && 
			(vaddr + memsize) > curr->base_addr) {
			return EINVAL;
		} else if (vaddr <= (curr->base_addr + curr->memsize) && 
			(vaddr + memsize) > (curr->base_addr + curr->memsize)) {
			return EINVAL;
		}
		curr = curr->next;
	}
	return 0;
}

/*
 * 1) Loop through the regions and free
 */
void regions_cleanup(struct addrspace *as) {
	struct region *temp;
	struct region *head = as->head;
	while(head != NULL) {
		temp = head;
		head = head->next;
		kfree(temp);
	}
}

/* PAGE TABLE HELPER FUNCTION */
int pt_copy(struct addrspace *old_as, paddr_t **old_pt, paddr_t **new_pt)
{
	for(int i = 0; i < PAGETABLE_SIZE; i++) {
		if(old_pt[i] != NULL) {
			new_pt[i] = kmalloc(PAGETABLE_SIZE * 4);
			if(new_pt[i] == NULL) {
				return ENOMEM;
			}
			/* copy everything inside */
			for(int j = 0; j < PAGETABLE_SIZE; j++)
			{
				if(old_pt[i][j] != EMPTY) {
					/* copy entry */
					/* allocate frame for this entry */
					vaddr_t frame_vaddr = alloc_frame(old_as, PADDR_TO_KVADDR(old_pt[i][j]));
					if(frame_vaddr == 0) return ENOMEM;
					/* copy contains in old mem into new mem */
					memmove((void *) frame_vaddr, (void *) PADDR_TO_KVADDR(old_pt[i][j] & PAGE_FRAME), PAGE_SIZE);
					new_pt[i][j] = KVADDR_TO_PADDR(frame_vaddr) & PAGE_FRAME;
					/* assign right bits */
					new_pt[i][j] = new_pt[i][j] | (TLBLO_DIRTY & old_pt[i][j]) | (TLBLO_VALID & old_pt[i][j]);
				} else {
					new_pt[i][j] = EMPTY;
				}
			}
		} else {
			new_pt[i] = NULL;
		}
		
	}
	return 0;
}


