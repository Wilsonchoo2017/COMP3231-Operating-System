#include <types.h>
#include <kern/errno.h>
#include <lib.h>
#include <thread.h>
#include <addrspace.h>
#include <synch.h>
#include <vm.h>
#include <proc.h>
#include <current.h>
#include <machine/tlb.h>
#include <spl.h>
/***************************************
 * Page Table Operation Functions
 ***************************************/
/* Insert Page Table entry
 * 1) Convert
 * 2) See if it is in a valid address space
 * 3) Link the Page Table Lvl 1 has reference to lvl 2
 * 4) Lvl 2 has ref to frame number
 * Assumed that paddr is already allocated a frame.
 */
int insert_pt(struct addrspace *as, vaddr_t vaddr, paddr_t paddr)
{
    uint32_t pd_bits = get_first_10_bits(vaddr);
    uint32_t pt_bits = get_middle_10_bits(vaddr);

    if (pd_bits >= PAGETABLE_SIZE || pt_bits >= PAGETABLE_SIZE) {
        return EFAULT; //TODO: Make sure this is correct error
    }

    lock_acquire(as->pt_lock);

    if (as->pagetable[pd_bits] == NULL) {
        as->pagetable[pd_bits] = kmalloc(PAGE_SIZE);
        if (as->pagetable[pd_bits] == NULL) {
            lock_release(as->pt_lock);
            return ENOMEM;
        }
        for(int i = 0; i < PAGETABLE_SIZE; i++) {
            as->pagetable[pd_bits][i] = EMPTY;
        }
    } else {
        // Check if there is something in page already
        if (as->pagetable[pd_bits][pt_bits] != EMPTY ){
            lock_release(as->pt_lock);
            return EFAULT; //TODO: Make sure this is correct error
        }
    }

    as->pagetable[pd_bits][pt_bits] = paddr;
    lock_release(as->pt_lock);

    return 0;
}


/* Look up Page Table entry
 * 1) Convert
 * 2) See if it is in a valid address space
 *      - FAILS return 0
 *      - SUCCESS return Frame No.
 */
paddr_t look_up_pt(struct addrspace *as, vaddr_t vaddr)
{
    KASSERT(0);
    uint32_t pd_bits = get_first_10_bits(vaddr);
    uint32_t pt_bits = get_middle_10_bits(vaddr);

    if (probe_pt(as, vaddr) != 0) {
        return EMPTY; //TODO: REPLACE with OUT OF RANGE
    }

    pd_bits = get_first_10_bits(vaddr);
    pt_bits = get_middle_10_bits(vaddr);

    return as->pagetable[pd_bits][pt_bits];
}
/*
 * look for an entry matching the virtual page
 * Returns the 0, or a negative number if no matching entry
 * was found. 
 */
int probe_pt(struct addrspace *as, vaddr_t vaddr) {
    uint32_t pd_bits = get_first_10_bits(vaddr);
    uint32_t pt_bits = get_middle_10_bits(vaddr);

    if (pd_bits >= PAGETABLE_SIZE || pt_bits >= PAGETABLE_SIZE) {
        return -1; //TODO: REPLACE with OUT OF RANGE
    }
    lock_acquire(as->pt_lock);
    if (as->pagetable == NULL) {
        lock_release(as->pt_lock);
        return -1;
    }
    if (as->pagetable[pd_bits] == NULL) {
        lock_release(as->pt_lock);
        return -1;
    }

    if (as->pagetable[pd_bits][pt_bits] == EMPTY) {
        lock_release(as->pt_lock);
        return -1;
    }

    lock_release(as->pt_lock);
    
    return 0;
}

/* Update Page Table Entry
 * 1) Convert
 * 2) Look up Page Table entry
 *      - FAILS return -1
 * 3) Update table
 */
int update_pt(struct addrspace *as, vaddr_t vaddr, paddr_t paddr)
{
    KASSERT(0);
    uint32_t result = probe_pt(as, vaddr);
    if (result) return result;

    uint32_t pd_bits = get_first_10_bits(vaddr);
    uint32_t pt_bits = get_middle_10_bits(vaddr);

    as->pagetable[pd_bits][pt_bits] = paddr;

    return 0;
}


void vm_bootstrap(void)
{
    /* Initialise VM sub-system.  You probably want to initialise your
       frame table here as well.
       NOTE: Frame Table already INIT and is already been done, no need to worry anything about it
       https://piazza.com/class/jrr04q7mah621s?cid=491
       Basically can do nothing???
    */
}


int vm_fault(int faulttype, vaddr_t faultaddress)
{
    /*
     * vm_fault is called when there is a page fault
     * Logic is define as following:
     * 1) check if address space is read only
     *      - YES: EFAULT
     *      - NO: move to next step
     * 2) Look up Page Table, check if PTE exist for this address.
     *      - YES: load to TLB and solve this fault (exit)
     *      - NO: move to next step
     * 3) Look the address space's region. check if it is a valid region.
     *      - YES: move to next step
     *      - NO: EFAULT
     * 4) Allocate Frame, Zero-filled, Insert PTE and then load TLB
     * Note: faultaddress is a virtual address, thus 10 bits = pd, 20 bits = pt
     */

    switch (faulttype)
    {
        case VM_FAULT_READONLY:
            return EFAULT;
        case VM_FAULT_WRITE:
        case VM_FAULT_READ:
            break;
        default:
            return EINVAL; /* Invalid Arg */
    }
    if (curproc == NULL) {
        /*
         * No process. This is probably a kernel fault early
         * in boot. Return EFAULT so as to panic instead of
         * getting into an infinite faulting loop.
         */
        return EFAULT;
    }



    struct addrspace *as = proc_getas(); /* get curproc's as to access our pt */
    if (as == NULL) {
        /*
         * No address space set up. This is probably also a
         * kernel fault early in boot.
         */
        return EFAULT;
    }
    paddr_t **pagetable = as->pagetable;
    if (pagetable == NULL) return EFAULT;
    uint32_t pd_bits = get_first_10_bits(faultaddress);
    uint32_t pt_bits = get_middle_10_bits(faultaddress);



    /* Look up Page Table */
    if (probe_pt(as, faultaddress) == 0)
    {
        /* get region and check bits */
        if (lookup_region(as, faultaddress, faulttype) == 0) {
            /* Load into TLB */
            load_tlb(faultaddress & PAGE_FRAME, pagetable[pd_bits][pt_bits]);
            return 0;
        }
        return EFAULT;
    }


    /* Check for valid Region
     * This means that we have to check whether its accessing a page that
     * should exist but have not been accessed yet
     */
    int result = lookup_region(as, faultaddress, faulttype); 
    if(result) {
        return result;
    }

    /* Found a region that is within process's region*/
    vaddr_t newVaddr = alloc_frame(as, faultaddress);
    if (newVaddr == 0) return ENOMEM;
    paddr_t paddr = KVADDR_TO_PADDR(newVaddr) & PAGE_FRAME;
    struct region *region = get_region(as, faultaddress);
    /* insert into PTE */
    if (region->writeable != 0) {
        paddr = paddr | TLBLO_DIRTY;
    }

    result = insert_pt(as, faultaddress, paddr | TLBLO_VALID);
    if (result != 0) {
        return ENOMEM;
    }
    /* Load TLB*/
    load_tlb(faultaddress & PAGE_FRAME, pagetable[pd_bits][pt_bits]);
    return 0;         /* return sucessfully */

}

/*
 *
 * SMP-specific functions.  Unused in our configuration.
 */

void
vm_tlbshootdown(const struct tlbshootdown *ts)
{
	(void)ts;
	panic("vm tried to do tlb shootdown?!\n");
}

/***************************************
 * Helper Functions
 ***************************************/
/* Save an entry into TLB */
void load_tlb(uint32_t entryhi, uint32_t entrylo)
{
    int sql = splhigh();
    tlb_random(entryhi, entrylo);
    splx(sql);
}

/*
 * Checks whether a region is valid
 * Iterate through all current process as's region and check whether its
 * within any of those regions.
 */
int lookup_region(struct addrspace *as, vaddr_t vaddr, int faulttype)
{
    struct region *curr = as->head;
    while(curr != NULL) {
        if (vaddr >= curr->base_addr &&
            (vaddr < (curr->base_addr + curr->memsize))) {
            break;
        }
        curr = curr->next;
    }

    if (curr == NULL)     return EFAULT; /* Cant find region thus return error */

    switch (faulttype)
    {
        case VM_FAULT_WRITE:
            if (curr->writeable == 0) return EPERM;
            break;
        case VM_FAULT_READ:
            if (curr->readable == 0) return EPERM;
            break;
        default:
            return EINVAL; /* Invalid Arg */
    }
    return 0;
}

struct region *get_region(struct addrspace *as, vaddr_t vaddr) {
    struct region *curr = as->head;
    while (curr != NULL) {
        if (vaddr >= curr->base_addr &&
            (vaddr < (curr->base_addr + curr->memsize))) {
            return curr;
        }
        curr = curr->next;
    }
    return NULL;
}

vaddr_t get_first_10_bits(vaddr_t addr) {
    //return KVADDR_TO_PADDR(addr) >> 22;
    return addr >> 22;
}

vaddr_t get_middle_10_bits(vaddr_t addr) {
    //return (KVADDR_TO_PADDR(addr) << 10) >> 22;
    return (addr << 10) >> 22;
}


vaddr_t alloc_frame(struct addrspace *as, vaddr_t vaddr) {
	/*  Allocate Frame for this region  */
    (void)as;
    vaddr = vaddr;
	//struct region *region = get_region(as, vaddr);
	//size_t regionSize = region->memsize;
	/* calculate how many frame number we need */
	//unsigned int frameNeeded = regionSize / PAGE_SIZE;
	//vaddr_t newVaddr = alloc_kpages(frameNeeded);
    vaddr_t newVaddr = alloc_kpages(1);
	if (newVaddr == 0) return 0;
	/* zero out the frame */
	//bzero((void *) newVaddr, regionSize);
    bzero((void *) newVaddr, PAGE_SIZE);
	return newVaddr;
}