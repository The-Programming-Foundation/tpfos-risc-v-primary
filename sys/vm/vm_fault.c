/*
 * Copyright (c) 1991, 1993
 *	The Regents of the University of California.  All rights reserved.
 * Copyright (c) 1994 John S. Dyson
 * All rights reserved.
 * Copyright (c) 1994 David Greenman
 * All rights reserved.
 *
 *
 * This code is derived from software contributed to Berkeley by
 * The Mach Operating System project at Carnegie-Mellon University.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *	This product includes software developed by the University of
 *	California, Berkeley and its contributors.
 * 4. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 *	from: @(#)vm_fault.c	8.4 (Berkeley) 1/12/94
 *
 *
 * Copyright (c) 1987, 1990 Carnegie-Mellon University.
 * All rights reserved.
 *
 * Authors: Avadis Tevanian, Jr., Michael Wayne Young
 *
 * Permission to use, copy, modify and distribute this software and
 * its documentation is hereby granted, provided that both the copyright
 * notice and this permission notice appear in all copies of the
 * software, derivative works or modified versions, and any portions
 * thereof, and that both notices appear in supporting documentation.
 *
 * CARNEGIE MELLON ALLOWS FREE USE OF THIS SOFTWARE IN ITS "AS IS"
 * CONDITION.  CARNEGIE MELLON DISCLAIMS ANY LIABILITY OF ANY KIND
 * FOR ANY DAMAGES WHATSOEVER RESULTING FROM THE USE OF THIS SOFTWARE.
 *
 * Carnegie Mellon requests users of this software to return to
 *
 *  Software Distribution Coordinator  or  Software.Distribution@CS.CMU.EDU
 *  School of Computer Science
 *  Carnegie Mellon University
 *  Pittsburgh PA 15213-3890
 *
 * any improvements or extensions that they make and grant Carnegie the
 * rights to redistribute these changes.
 *
 * $Id: vm_fault.c,v 1.88 1998/09/04 08:06:57 dfr Exp $
 */

/*
 *	Page fault handling module.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/proc.h>
#include <sys/vnode.h>
#include <sys/resourcevar.h>
#include <sys/vmmeter.h>

#include <vm/vm.h>
#include <vm/vm_param.h>
#include <vm/vm_prot.h>
#include <sys/lock.h>
#include <vm/pmap.h>
#include <vm/vm_map.h>
#include <vm/vm_object.h>
#include <vm/vm_page.h>
#include <vm/vm_pageout.h>
#include <vm/vm_kern.h>
#include <vm/vm_pager.h>
#include <vm/vnode_pager.h>
#include <vm/vm_extern.h>

static int vm_fault_additional_pages __P((vm_page_t, int,
					  int, vm_page_t *, int *));

#define VM_FAULT_READ_AHEAD 8
#define VM_FAULT_READ_BEHIND 7
#define VM_FAULT_READ (VM_FAULT_READ_AHEAD+VM_FAULT_READ_BEHIND+1)

struct faultstate {
	vm_page_t m;
	vm_object_t object;
	vm_pindex_t pindex;
	vm_page_t first_m;
	vm_object_t	first_object;
	vm_pindex_t first_pindex;
	vm_map_t map;
	vm_map_entry_t entry;
	int lookup_still_valid;
	struct vnode *vp;
};

static void
release_page(struct faultstate *fs)
{
	vm_page_wakeup(fs->m);
	vm_page_deactivate(fs->m);
	fs->m = NULL;
}

static void
unlock_map(struct faultstate *fs)
{
	if (fs->lookup_still_valid) {
		vm_map_lookup_done(fs->map, fs->entry);
		fs->lookup_still_valid = FALSE;
	}
}

static void
_unlock_things(struct faultstate *fs, int dealloc)
{
	vm_object_pip_wakeup(fs->object);
	if (fs->object != fs->first_object) {
		vm_page_free(fs->first_m);
		vm_object_pip_wakeup(fs->first_object);
		fs->first_m = NULL;
	}
	if (dealloc) {
		vm_object_deallocate(fs->first_object);
	}
	unlock_map(fs);	
	if (fs->vp != NULL) { 
		vput(fs->vp);
		fs->vp = NULL;
	}
}

#define unlock_things(fs) _unlock_things(fs, 0)
#define unlock_and_deallocate(fs) _unlock_things(fs, 1)

/*
 *	vm_fault:
 *
 *	Handle a page fault occuring at the given address,
 *	requiring the given permissions, in the map specified.
 *	If successful, the page is inserted into the
 *	associated physical map.
 *
 *	NOTE: the given address should be truncated to the
 *	proper page address.
 *
 *	KERN_SUCCESS is returned if the page fault is handled; otherwise,
 *	a standard error specifying why the fault is fatal is returned.
 *
 *
 *	The map in question must be referenced, and remains so.
 *	Caller may hold no locks.
 */
int
vm_fault(vm_map_t map, vm_offset_t vaddr, vm_prot_t fault_type, int fault_flags)
{
	vm_prot_t prot;
	int result;
	boolean_t wired;
	int map_generation;
	vm_page_t old_m;
	vm_object_t next_object;
	vm_page_t marray[VM_FAULT_READ];
	int hardfault;
	int faultcount;
	struct proc *p = curproc;	/* XXX */
	struct faultstate fs;

	cnt.v_vm_faults++;	/* needs lock XXX */
	hardfault = 0;

RetryFault:;
	fs.map = map;

	/*
	 * Find the backing store object and offset into it to begin the
	 * search.
	 */
	if ((result = vm_map_lookup(&fs.map, vaddr,
		fault_type, &fs.entry, &fs.first_object,
		&fs.first_pindex, &prot, &wired)) != KERN_SUCCESS) {
		if ((result != KERN_PROTECTION_FAILURE) ||
			((fault_flags & VM_FAULT_WIRE_MASK) != VM_FAULT_USER_WIRE)) {
			return result;
		}

		/*
   		 * If we are user-wiring a r/w segment, and it is COW, then
   		 * we need to do the COW operation.  Note that we don't COW
   		 * currently RO sections now, because it is NOT desirable
   		 * to COW .text.  We simply keep .text from ever being COW'ed
   		 * and take the heat that one cannot debug wired .text sections.
   		 */
		result = vm_map_lookup(&fs.map, vaddr,
			VM_PROT_READ|VM_PROT_WRITE|VM_PROT_OVERRIDE_WRITE,
			&fs.entry, &fs.first_object, &fs.first_pindex, &prot, &wired);
		if (result != KERN_SUCCESS) {
			return result;
		}

		/*
		 * If we don't COW now, on a user wire, the user will never
		 * be able to write to the mapping.  If we don't make this
		 * restriction, the bookkeeping would be nearly impossible.
		 */
		if ((fs.entry->protection & VM_PROT_WRITE) == 0)
			fs.entry->max_protection &= ~VM_PROT_WRITE;
	}

	map_generation = fs.map->timestamp;

	if (fs.entry->eflags & MAP_ENTRY_NOFAULT) {
		panic("vm_fault: fault on nofault entry, addr: %lx",
		    (u_long)vaddr);
	}

	/*
	 * Make a reference to this object to prevent its disposal while we
	 * are messing with it.  Once we have the reference, the map is free
	 * to be diddled.  Since objects reference their shadows (and copies),
	 * they will stay around as well.
	 */
	vm_object_reference(fs.first_object);
	vm_object_pip_add(fs.first_object, 1);

	fs.vp = vnode_pager_lock(fs.first_object);
	if ((fault_type & VM_PROT_WRITE) &&
		(fs.first_object->type == OBJT_VNODE)) {
		vm_freeze_copyopts(fs.first_object,
			fs.first_pindex, fs.first_pindex + 1);
	}

	fs.lookup_still_valid = TRUE;

	if (wired)
		fault_type = prot;

	fs.first_m = NULL;

	/*
	 * Search for the page at object/offset.
	 */

	fs.object = fs.first_object;
	fs.pindex = fs.first_pindex;

	/*
	 * See whether this page is resident
	 */
	while (TRUE) {

		if (fs.object->flags & OBJ_DEAD) {
			unlock_and_deallocate(&fs);
			return (KERN_PROTECTION_FAILURE);
		}
			
		fs.m = vm_page_lookup(fs.object, fs.pindex);
		if (fs.m != NULL) {
			int queue;
			/*
			 * If the page is being brought in, wait for it and
			 * then retry.
			 */
			if ((fs.m->flags & PG_BUSY) ||
				(fs.m->busy &&
				 (fs.m->valid & VM_PAGE_BITS_ALL) != VM_PAGE_BITS_ALL)) {
				int s;

				unlock_things(&fs);
				s = splvm();
				if ((fs.m->flags & PG_BUSY) ||
					(fs.m->busy &&
					 (fs.m->valid & VM_PAGE_BITS_ALL) != VM_PAGE_BITS_ALL)) {
					vm_page_flag_set(fs.m, PG_WANTED | PG_REFERENCED);
					cnt.v_intrans++;
					tsleep(fs.m, PSWP, "vmpfw", 0);
				}
				splx(s);
				vm_object_deallocate(fs.first_object);
				goto RetryFault;
			}

			queue = fs.m->queue;
			vm_page_unqueue_nowakeup(fs.m);

			/*
			 * Mark page busy for other processes, and the pagedaemon.
			 */
			if (((queue - fs.m->pc) == PQ_CACHE) &&
			    (cnt.v_free_count + cnt.v_cache_count) < cnt.v_free_min) {
				vm_page_activate(fs.m);
				unlock_and_deallocate(&fs);
				VM_WAIT;
				goto RetryFault;
			}

			vm_page_busy(fs.m);
			if (((fs.m->valid & VM_PAGE_BITS_ALL) != VM_PAGE_BITS_ALL) &&
				fs.m->object != kernel_object && fs.m->object != kmem_object) {
				goto readrest;
			}

			break;
		}
		if (((fs.object->type != OBJT_DEFAULT) &&
				(((fault_flags & VM_FAULT_WIRE_MASK) == 0) || wired))
		    || (fs.object == fs.first_object)) {

			if (fs.pindex >= fs.object->size) {
				unlock_and_deallocate(&fs);
				return (KERN_PROTECTION_FAILURE);
			}

			/*
			 * Allocate a new page for this object/offset pair.
			 */
			fs.m = vm_page_alloc(fs.object, fs.pindex,
				(fs.vp || fs.object->backing_object)? VM_ALLOC_NORMAL: VM_ALLOC_ZERO);

			if (fs.m == NULL) {
				unlock_and_deallocate(&fs);
				VM_WAIT;
				goto RetryFault;
			}
		}

readrest:
		if (fs.object->type != OBJT_DEFAULT &&
			(((fault_flags & VM_FAULT_WIRE_MASK) == 0) || wired)) {
			int rv;
			int reqpage;
			int ahead, behind;

			if (fs.first_object->behavior == OBJ_RANDOM) {
				ahead = 0;
				behind = 0;
			} else {
				behind = (vaddr - fs.entry->start) >> PAGE_SHIFT;
				if (behind > VM_FAULT_READ_BEHIND)
					behind = VM_FAULT_READ_BEHIND;

				ahead = ((fs.entry->end - vaddr) >> PAGE_SHIFT) - 1;
				if (ahead > VM_FAULT_READ_AHEAD)
					ahead = VM_FAULT_READ_AHEAD;
			}

			if ((fs.first_object->type != OBJT_DEVICE) &&
				(fs.first_object->behavior == OBJ_SEQUENTIAL)) {
				vm_pindex_t firstpindex, tmppindex;
				if (fs.first_pindex <
					2*(VM_FAULT_READ_BEHIND + VM_FAULT_READ_AHEAD + 1))
					firstpindex = 0;
				else
					firstpindex = fs.first_pindex -
						2*(VM_FAULT_READ_BEHIND + VM_FAULT_READ_AHEAD + 1);

				for(tmppindex = fs.first_pindex - 1;
					tmppindex >= firstpindex;
					--tmppindex) {
					vm_page_t mt;
					mt = vm_page_lookup( fs.first_object, tmppindex);
					if (mt == NULL || (mt->valid != VM_PAGE_BITS_ALL))
						break;
					if (mt->busy ||
						(mt->flags & (PG_BUSY | PG_FICTITIOUS)) ||
						mt->hold_count ||
						mt->wire_count) 
						continue;
					if (mt->dirty == 0)
						vm_page_test_dirty(mt);
					if (mt->dirty) {
						vm_page_protect(mt, VM_PROT_NONE);
						vm_page_deactivate(mt);
					} else {
						vm_page_cache(mt);
					}
				}

				ahead += behind;
				behind = 0;
			}

			/*
			 * now we find out if any other pages should be paged
			 * in at this time this routine checks to see if the
			 * pages surrounding this fault reside in the same
			 * object as the page for this fault.  If they do,
			 * then they are faulted in also into the object.  The
			 * array "marray" returned contains an array of
			 * vm_page_t structs where one of them is the
			 * vm_page_t passed to the routine.  The reqpage
			 * return value is the index into the marray for the
			 * vm_page_t passed to the routine.
			 */
			faultcount = vm_fault_additional_pages(
			    fs.m, behind, ahead, marray, &reqpage);

			/*
			 * Call the pager to retrieve the data, if any, after
			 * releasing the lock on the map.
			 */
			unlock_map(&fs);

			rv = faultcount ?
			    vm_pager_get_pages(fs.object, marray, faultcount,
				reqpage) : VM_PAGER_FAIL;

			if (rv == VM_PAGER_OK) {
				/*
				 * Found the page. Leave it busy while we play
				 * with it.
				 */

				/*
				 * Relookup in case pager changed page. Pager
				 * is responsible for disposition of old page
				 * if moved.
				 */
				fs.m = vm_page_lookup(fs.object, fs.pindex);
				if(!fs.m) {
					unlock_and_deallocate(&fs);
					goto RetryFault;
				}

				hardfault++;
				break;
			}
			/*
			 * Remove the bogus page (which does not exist at this
			 * object/offset); before doing so, we must get back
			 * our object lock to preserve our invariant.
			 *
			 * Also wake up any other process that may want to bring
			 * in this page.
			 *
			 * If this is the top-level object, we must leave the
			 * busy page to prevent another process from rushing
			 * past us, and inserting the page in that object at
			 * the same time that we are.
			 */

			if (rv == VM_PAGER_ERROR)
				printf("vm_fault: pager read error, pid %d (%s)\n",
				    curproc->p_pid, curproc->p_comm);
			/*
			 * Data outside the range of the pager or an I/O error
			 */
			/*
			 * XXX - the check for kernel_map is a kludge to work
			 * around having the machine panic on a kernel space
			 * fault w/ I/O error.
			 */
			if (((fs.map != kernel_map) && (rv == VM_PAGER_ERROR)) ||
				(rv == VM_PAGER_BAD)) {
				vm_page_free(fs.m);
				fs.m = NULL;
				unlock_and_deallocate(&fs);
				return ((rv == VM_PAGER_ERROR) ? KERN_FAILURE : KERN_PROTECTION_FAILURE);
			}
			if (fs.object != fs.first_object) {
				vm_page_free(fs.m);
				fs.m = NULL;
				/*
				 * XXX - we cannot just fall out at this
				 * point, m has been freed and is invalid!
				 */
			}
		}
		/*
		 * We get here if the object has default pager (or unwiring) or the
		 * pager doesn't have the page.
		 */
		if (fs.object == fs.first_object)
			fs.first_m = fs.m;

		/*
		 * Move on to the next object.  Lock the next object before
		 * unlocking the current one.
		 */

		fs.pindex += OFF_TO_IDX(fs.object->backing_object_offset);
		next_object = fs.object->backing_object;
		if (next_object == NULL) {
			/*
			 * If there's no object left, fill the page in the top
			 * object with zeros.
			 */
			if (fs.object != fs.first_object) {
				vm_object_pip_wakeup(fs.object);

				fs.object = fs.first_object;
				fs.pindex = fs.first_pindex;
				fs.m = fs.first_m;
			}
			fs.first_m = NULL;

			if ((fs.m->flags & PG_ZERO) == 0) {
				vm_page_zero_fill(fs.m);
				cnt.v_ozfod++;
			}
			cnt.v_zfod++;
			break;
		} else {
			if (fs.object != fs.first_object) {
				vm_object_pip_wakeup(fs.object);
			}
			fs.object = next_object;
			vm_object_pip_add(fs.object, 1);
		}
	}

#if defined(DIAGNOSTIC)
	if ((fs.m->flags & PG_BUSY) == 0)
		panic("vm_fault: not busy after main loop");
#endif

	/*
	 * PAGE HAS BEEN FOUND. [Loop invariant still holds -- the object lock
	 * is held.]
	 */

	old_m = fs.m;	/* save page that would be copied */

	/*
	 * If the page is being written, but isn't already owned by the
	 * top-level object, we have to copy it into a new page owned by the
	 * top-level object.
	 */

	if (fs.object != fs.first_object) {
		/*
		 * We only really need to copy if we want to write it.
		 */

		if (fault_type & VM_PROT_WRITE) {

			/*
			 * This allows pages to be virtually copied from a backing_object
			 * into the first_object, where the backing object has no other
			 * refs to it, and cannot gain any more refs.  Instead of a
			 * bcopy, we just move the page from the backing object to the
			 * first object.  Note that we must mark the page dirty in the
			 * first object so that it will go out to swap when needed.
			 */
			if (map_generation == fs.map->timestamp &&
				/*
				 * Only one shadow object
				 */
				(fs.object->shadow_count == 1) &&
				/*
				 * No COW refs, except us
				 */
				(fs.object->ref_count == 1) &&
				/*
				 * Noone else can look this object up
				 */
				(fs.object->handle == NULL) &&
				/*
				 * No other ways to look the object up
				 */
				((fs.object->type == OBJT_DEFAULT) ||
				 (fs.object->type == OBJT_SWAP)) &&
				/*
				 * We don't chase down the shadow chain
				 */
				(fs.object == fs.first_object->backing_object) &&

				/*
				 * grab the lock if we need to
				 */
				(fs.lookup_still_valid ||
						(((fs.entry->eflags & MAP_ENTRY_IS_A_MAP) == 0) &&
						 lockmgr(&fs.map->lock,
							LK_EXCLUSIVE|LK_NOWAIT, (void *)0, curproc) == 0))) {
				
				fs.lookup_still_valid = 1;
				/*
				 * get rid of the unnecessary page
				 */
				vm_page_protect(fs.first_m, VM_PROT_NONE);
				vm_page_free(fs.first_m);
				fs.first_m = NULL;

				/*
				 * grab the page and put it into the process'es object
				 */
				vm_page_rename(fs.m, fs.first_object, fs.first_pindex);
				fs.first_m = fs.m;
				fs.first_m->dirty = VM_PAGE_BITS_ALL;
				vm_page_busy(fs.first_m);
				fs.m = NULL;
				cnt.v_cow_optim++;
			} else {
				/*
				 * Oh, well, lets copy it.
				 */
				vm_page_copy(fs.m, fs.first_m);
			}

			if (fs.m) {
				/*
				 * We no longer need the old page or object.
				 */
				release_page(&fs);
			}

			vm_object_pip_wakeup(fs.object);
			/*
			 * Only use the new page below...
			 */

			cnt.v_cow_faults++;
			fs.m = fs.first_m;
			fs.object = fs.first_object;
			fs.pindex = fs.first_pindex;

		} else {
			prot &= ~VM_PROT_WRITE;
		}
	}

	/*
	 * We must verify that the maps have not changed since our last
	 * lookup.
	 */

	if (!fs.lookup_still_valid &&
		(fs.map->timestamp != map_generation)) {
		vm_object_t retry_object;
		vm_pindex_t retry_pindex;
		vm_prot_t retry_prot;

		/*
		 * Since map entries may be pageable, make sure we can take a
		 * page fault on them.
		 */

		/*
		 * To avoid trying to write_lock the map while another process
		 * has it read_locked (in vm_map_pageable), we do not try for
		 * write permission.  If the page is still writable, we will
		 * get write permission.  If it is not, or has been marked
		 * needs_copy, we enter the mapping without write permission,
		 * and will merely take another fault.
		 */
		result = vm_map_lookup(&fs.map, vaddr, fault_type & ~VM_PROT_WRITE,
		    &fs.entry, &retry_object, &retry_pindex, &retry_prot, &wired);
		map_generation = fs.map->timestamp;

		/*
		 * If we don't need the page any longer, put it on the active
		 * list (the easiest thing to do here).  If no one needs it,
		 * pageout will grab it eventually.
		 */

		if (result != KERN_SUCCESS) {
			release_page(&fs);
			unlock_and_deallocate(&fs);
			return (result);
		}
		fs.lookup_still_valid = TRUE;

		if ((retry_object != fs.first_object) ||
		    (retry_pindex != fs.first_pindex)) {
			release_page(&fs);
			unlock_and_deallocate(&fs);
			goto RetryFault;
		}
		/*
		 * Check whether the protection has changed or the object has
		 * been copied while we left the map unlocked. Changing from
		 * read to write permission is OK - we leave the page
		 * write-protected, and catch the write fault. Changing from
		 * write to read permission means that we can't mark the page
		 * write-enabled after all.
		 */
		prot &= retry_prot;
	}

	/*
	 * Put this page into the physical map. We had to do the unlock above
	 * because pmap_enter may cause other faults.   We don't put the page
	 * back on the active queue until later so that the page-out daemon
	 * won't find us (yet).
	 */

	if (prot & VM_PROT_WRITE) {
		vm_page_flag_set(fs.m, PG_WRITEABLE);
		vm_object_set_flag(fs.m->object,
				   OBJ_WRITEABLE|OBJ_MIGHTBEDIRTY);
		/*
		 * If the fault is a write, we know that this page is being
		 * written NOW. This will save on the pmap_is_modified() calls
		 * later.
		 */
		if (fault_flags & VM_FAULT_DIRTY) {
			fs.m->dirty = VM_PAGE_BITS_ALL;
		}
	}

	unlock_things(&fs);
	fs.m->valid = VM_PAGE_BITS_ALL;
	vm_page_flag_clear(fs.m, PG_ZERO);

	pmap_enter(fs.map->pmap, vaddr, VM_PAGE_TO_PHYS(fs.m), prot, wired);
	if (((fault_flags & VM_FAULT_WIRE_MASK) == 0) && (wired == 0)) {
		pmap_prefault(fs.map->pmap, vaddr, fs.entry);
	}

	vm_page_flag_set(fs.m, PG_MAPPED|PG_REFERENCED);
	if (fault_flags & VM_FAULT_HOLD)
		vm_page_hold(fs.m);

	/*
	 * If the page is not wired down, then put it where the pageout daemon
	 * can find it.
	 */
	if (fault_flags & VM_FAULT_WIRE_MASK) {
		if (wired)
			vm_page_wire(fs.m);
		else
			vm_page_unwire(fs.m);
	} else {
		vm_page_activate(fs.m);
	}

	if (curproc && (curproc->p_flag & P_INMEM) && curproc->p_stats) {
		if (hardfault) {
			curproc->p_stats->p_ru.ru_majflt++;
		} else {
			curproc->p_stats->p_ru.ru_minflt++;
		}
	}

	/*
	 * Unlock everything, and return
	 */

	vm_page_wakeup(fs.m);
	vm_object_deallocate(fs.first_object);

	return (KERN_SUCCESS);

}

/*
 *	vm_fault_wire:
 *
 *	Wire down a range of virtual addresses in a map.
 */
int
vm_fault_wire(map, start, end)
	vm_map_t map;
	vm_offset_t start, end;
{

	register vm_offset_t va;
	register pmap_t pmap;
	int rv;

	pmap = vm_map_pmap(map);

	/*
	 * Inform the physical mapping system that the range of addresses may
	 * not fault, so that page tables and such can be locked down as well.
	 */

	pmap_pageable(pmap, start, end, FALSE);

	/*
	 * We simulate a fault to get the page and enter it in the physical
	 * map.
	 */

	for (va = start; va < end; va += PAGE_SIZE) {
		rv = vm_fault(map, va, VM_PROT_READ|VM_PROT_WRITE,
			VM_FAULT_CHANGE_WIRING);
		if (rv) {
			if (va != start)
				vm_fault_unwire(map, start, va);
			return (rv);
		}
	}
	return (KERN_SUCCESS);
}

/*
 *	vm_fault_user_wire:
 *
 *	Wire down a range of virtual addresses in a map.  This
 *	is for user mode though, so we only ask for read access
 *	on currently read only sections.
 */
int
vm_fault_user_wire(map, start, end)
	vm_map_t map;
	vm_offset_t start, end;
{

	register vm_offset_t va;
	register pmap_t pmap;
	int rv;

	pmap = vm_map_pmap(map);

	/*
	 * Inform the physical mapping system that the range of addresses may
	 * not fault, so that page tables and such can be locked down as well.
	 */

	pmap_pageable(pmap, start, end, FALSE);

	/*
	 * We simulate a fault to get the page and enter it in the physical
	 * map.
	 */
	for (va = start; va < end; va += PAGE_SIZE) {
		rv = vm_fault(map, va, VM_PROT_READ, VM_FAULT_USER_WIRE);
		if (rv) {
			if (va != start)
				vm_fault_unwire(map, start, va);
			return (rv);
		}
	}
	return (KERN_SUCCESS);
}


/*
 *	vm_fault_unwire:
 *
 *	Unwire a range of virtual addresses in a map.
 */
void
vm_fault_unwire(map, start, end)
	vm_map_t map;
	vm_offset_t start, end;
{

	register vm_offset_t va, pa;
	register pmap_t pmap;

	pmap = vm_map_pmap(map);

	/*
	 * Since the pages are wired down, we must be able to get their
	 * mappings from the physical map system.
	 */

	for (va = start; va < end; va += PAGE_SIZE) {
		pa = pmap_extract(pmap, va);
		if (pa != (vm_offset_t) 0) {
			pmap_change_wiring(pmap, va, FALSE);
			vm_page_unwire(PHYS_TO_VM_PAGE(pa));
		}
	}

	/*
	 * Inform the physical mapping system that the range of addresses may
	 * fault, so that page tables and such may be unwired themselves.
	 */

	pmap_pageable(pmap, start, end, TRUE);

}

/*
 *	Routine:
 *		vm_fault_copy_entry
 *	Function:
 *		Copy all of the pages from a wired-down map entry to another.
 *
 *	In/out conditions:
 *		The source and destination maps must be locked for write.
 *		The source map entry must be wired down (or be a sharing map
 *		entry corresponding to a main map entry that is wired down).
 */

void
vm_fault_copy_entry(dst_map, src_map, dst_entry, src_entry)
	vm_map_t dst_map;
	vm_map_t src_map;
	vm_map_entry_t dst_entry;
	vm_map_entry_t src_entry;
{
	vm_object_t dst_object;
	vm_object_t src_object;
	vm_ooffset_t dst_offset;
	vm_ooffset_t src_offset;
	vm_prot_t prot;
	vm_offset_t vaddr;
	vm_page_t dst_m;
	vm_page_t src_m;

#ifdef	lint
	src_map++;
#endif	/* lint */

	src_object = src_entry->object.vm_object;
	src_offset = src_entry->offset;

	/*
	 * Create the top-level object for the destination entry. (Doesn't
	 * actually shadow anything - we copy the pages directly.)
	 */
	dst_object = vm_object_allocate(OBJT_DEFAULT,
	    (vm_size_t) OFF_TO_IDX(dst_entry->end - dst_entry->start));

	dst_entry->object.vm_object = dst_object;
	dst_entry->offset = 0;

	prot = dst_entry->max_protection;

	/*
	 * Loop through all of the pages in the entry's range, copying each
	 * one from the source object (it should be there) to the destination
	 * object.
	 */
	for (vaddr = dst_entry->start, dst_offset = 0;
	    vaddr < dst_entry->end;
	    vaddr += PAGE_SIZE, dst_offset += PAGE_SIZE) {

		/*
		 * Allocate a page in the destination object
		 */
		do {
			dst_m = vm_page_alloc(dst_object,
				OFF_TO_IDX(dst_offset), VM_ALLOC_NORMAL);
			if (dst_m == NULL) {
				VM_WAIT;
			}
		} while (dst_m == NULL);

		/*
		 * Find the page in the source object, and copy it in.
		 * (Because the source is wired down, the page will be in
		 * memory.)
		 */
		src_m = vm_page_lookup(src_object,
			OFF_TO_IDX(dst_offset + src_offset));
		if (src_m == NULL)
			panic("vm_fault_copy_wired: page missing");

		vm_page_copy(src_m, dst_m);

		/*
		 * Enter it in the pmap...
		 */

		vm_page_flag_clear(dst_m, PG_ZERO);
		pmap_enter(dst_map->pmap, vaddr, VM_PAGE_TO_PHYS(dst_m),
		    prot, FALSE);
		vm_page_flag_set(dst_m, PG_WRITEABLE|PG_MAPPED);

		/*
		 * Mark it no longer busy, and put it on the active list.
		 */
		vm_page_activate(dst_m);
		vm_page_wakeup(dst_m);
	}
}


/*
 * This routine checks around the requested page for other pages that
 * might be able to be faulted in.  This routine brackets the viable
 * pages for the pages to be paged in.
 *
 * Inputs:
 *	m, rbehind, rahead
 *
 * Outputs:
 *  marray (array of vm_page_t), reqpage (index of requested page)
 *
 * Return value:
 *  number of pages in marray
 */
static int
vm_fault_additional_pages(m, rbehind, rahead, marray, reqpage)
	vm_page_t m;
	int rbehind;
	int rahead;
	vm_page_t *marray;
	int *reqpage;
{
	int i,j;
	vm_object_t object;
	vm_pindex_t pindex, startpindex, endpindex, tpindex;
	vm_page_t rtm;
	int cbehind, cahead;

	object = m->object;
	pindex = m->pindex;

	/*
	 * we don't fault-ahead for device pager
	 */
	if (object->type == OBJT_DEVICE) {
		*reqpage = 0;
		marray[0] = m;
		return 1;
	}

	/*
	 * if the requested page is not available, then give up now
	 */

	if (!vm_pager_has_page(object,
		OFF_TO_IDX(object->paging_offset) + pindex, &cbehind, &cahead)) {
		return 0;
	}

	if ((cbehind == 0) && (cahead == 0)) {
		*reqpage = 0;
		marray[0] = m;
		return 1;
	}

	if (rahead > cahead) {
		rahead = cahead;
	}

	if (rbehind > cbehind) {
		rbehind = cbehind;
	}

	/*
	 * try to do any readahead that we might have free pages for.
	 */
	if ((rahead + rbehind) >
		((cnt.v_free_count + cnt.v_cache_count) - cnt.v_free_reserved)) {
		pagedaemon_wakeup();
		marray[0] = m;
		*reqpage = 0;
		return 1;
	}

	/*
	 * scan backward for the read behind pages -- in memory 
	 */
	if (pindex > 0) {
		if (rbehind > pindex) {
			rbehind = pindex;
			startpindex = 0;
		} else {
			startpindex = pindex - rbehind;
		}

		for ( tpindex = pindex - 1; tpindex >= startpindex; tpindex -= 1) {
			if (vm_page_lookup( object, tpindex)) {
				startpindex = tpindex + 1;
				break;
			}
			if (tpindex == 0)
				break;
		}

		for(i = 0, tpindex = startpindex; tpindex < pindex; i++, tpindex++) {

			rtm = vm_page_alloc(object, tpindex, VM_ALLOC_NORMAL);
			if (rtm == NULL) {
				for (j = 0; j < i; j++) {
					vm_page_free(marray[j]);
				}
				marray[0] = m;
				*reqpage = 0;
				return 1;
			}

			marray[i] = rtm;
		}
	} else {
		startpindex = 0;
		i = 0;
	}

	marray[i] = m;
	/* page offset of the required page */
	*reqpage = i;

	tpindex = pindex + 1;
	i++;

	/*
	 * scan forward for the read ahead pages
	 */
	endpindex = tpindex + rahead;
	if (endpindex > object->size)
		endpindex = object->size;

	for( ; tpindex < endpindex; i++, tpindex++) {

		if (vm_page_lookup(object, tpindex)) {
			break;
		}

		rtm = vm_page_alloc(object, tpindex, VM_ALLOC_NORMAL);
		if (rtm == NULL) {
			break;
		}

		marray[i] = rtm;
	}

	/* return number of bytes of pages */
	return i;
}
