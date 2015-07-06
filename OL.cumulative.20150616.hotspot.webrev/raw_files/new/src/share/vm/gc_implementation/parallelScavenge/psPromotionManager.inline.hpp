/*
 * Copyright (c) 2002, 2014, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 *
 */

#ifndef SHARE_VM_GC_IMPLEMENTATION_PARALLELSCAVENGE_PSPROMOTIONMANAGER_INLINE_HPP
#define SHARE_VM_GC_IMPLEMENTATION_PARALLELSCAVENGE_PSPROMOTIONMANAGER_INLINE_HPP

#include "gc_implementation/parallelScavenge/psOldGen.hpp"
#include "gc_implementation/parallelScavenge/psPromotionManager.hpp"
#include "gc_implementation/parallelScavenge/psPromotionLAB.inline.hpp"
#include "gc_implementation/parallelScavenge/psScavenge.hpp"
#include "oops/oop.psgc.inline.hpp"

inline PSPromotionManager* PSPromotionManager::manager_array(int index) {
  assert(_manager_array != NULL, "access of NULL manager_array");
  assert(index >= 0 && index <= (int)ParallelGCThreads, "out of range manager_array access");
  return &_manager_array[index];
}

template <class T>
inline void PSPromotionManager::claim_or_forward_depth(T* p) {
  assert(PSScavenge::should_scavenge(p, true), "revisiting object?");
  assert(Universe::heap()->kind() == CollectedHeap::ParallelScavengeHeap,
      "Sanity");
  assert(Universe::heap()->is_in(p), "pointer outside heap");

  if (p != NULL) {
    oop o = oopDesc::load_decode_heap_oop_not_null(p);
    if (o->is_forwarded()) {
      o = o->forwardee();
      if (PSScavenge::is_obj_in_young(o)) {
        PSScavenge::card_table()->inline_write_ref_field_gc(p, o);
      }
      oopDesc::encode_store_heap_oop_not_null(p, o);
    } else {
      push_depth(p);
    }
  }
}

// This method is pretty bulky. It would be nice to split it up into smaller
// submethods, but we need to be careful not to hurt performance.
template<bool promote_immediately>
oop PSPromotionManager::copy_to_survivor_space(oop o) {
  assert(PSScavenge::should_scavenge(&o), "sanity");

  // This method is not called if o is known to be already forwarded. The
  // corresponding check is done by the caller just before this call.

  oop new_obj = NULL;
  oop original_obj = o;

  // Evacuating a live contained object triggers immediate evacuation of
  // its outermost container. At this point, if the outermost container is not
  // marked yet, we cannot definitely say whether it is alive or not. We always
  // treat is as alive one. This also means that "the object is contained"
  // property never changes during Scavenge.
  bool need_container_copying = o->is_contained();
  jlong offset_from_outermost_container = 0;
  if (need_container_copying) {
    o = o->outermost_container();
    offset_from_outermost_container =
        (jlong) (((address) original_obj) - ((address) o));
  }

  // NOTE! We must be very careful with any methods that access the mark in o.
  // There may be multiple threads racing on it, and it may be forwarded
  // at any time. Do not use oopDesc methods for accessing the mark!
  markOop test_mark = o->mark();

  // Check whether the object is already forwarded
  if (!test_mark->is_marked()) {
    bool new_obj_is_tenured = false;
    size_t new_obj_size = o->size();

    if (oopDesc::is_container(test_mark, o->klass())) {
      new_obj_size = org_ObjectLayout_AbstractStructuredArray::
          footprint_with_contained_objects(o) >> LogHeapWordSize;
    }

    if (!promote_immediately) {
      // Find the object's age in MT safe way
      uint age = test_mark->has_displaced_mark_helper() ?
          test_mark->displaced_mark_helper()->age() : test_mark->age();

      // Try to allocate the object in To space (unless it is too old)
      if (age < PSScavenge::tenuring_threshold()) {
        new_obj = (oop) _young_lab.allocate(new_obj_size);
        if (new_obj == NULL) {
          new_obj = allocate_in_young_gen_slow(new_obj_size);
        }
      }
    }

    // Otherwise try to allocate the object in the old generation
    if (new_obj == NULL) {
#ifndef PRODUCT
      if (Universe::heap()->promotion_should_fail()) {
        new_obj = oop_promotion_failed(o, test_mark);

        if (need_container_copying) {
          new_obj =
              (oop) (((address) new_obj) + offset_from_outermost_container);
        }

        return new_obj;
      }
#endif // PRODUCT

      new_obj_is_tenured = true;
      // TODO: Update object start array
      new_obj = (oop) _old_lab.allocate(new_obj_size);
      if (new_obj == NULL) {
        new_obj = allocate_in_old_gen_slow(new_obj_size);

        // This is the promotion failed test, and code handling.
        // The code belongs here for two reasons. It is slightly
        // different than the code below, and cannot share the
        // CAS testing code. Keeping the code here also minimizes
        // the impact on the common case fast path code.

        if (new_obj == NULL) {
          new_obj = oop_promotion_failed(o, test_mark);

          if (need_container_copying) {
            new_obj =
                (oop) (((address) new_obj) + offset_from_outermost_container);
          }

          return new_obj;
        }
      }
    }

    assert(new_obj != NULL, "allocation should have succeeded");

    // Copy the object
    Copy::aligned_disjoint_words((HeapWord*) o, (HeapWord*) new_obj,
        new_obj_size);

    // Now we have to CAS in the header.
    if (o->cas_forward_to(new_obj, test_mark)) {
      // We won any races, we "own" this object.
      assert(new_obj == o->forwardee(), "sanity");

      // Now that we're dealing with a markOop that cannot change, it is okay
      // to use the non MT safe oopDesc methods.

      // Increment age if the object is still in the young generation
      if (!new_obj_is_tenured) {
        assert(young_space()->contains(new_obj),
            "object must belong to To space");
        new_obj->incr_age();
      }

#ifndef PRODUCT
      if (TraceScavenge) {
        ResourceMark rm;
        gclog_or_tty->print_cr("{%s %s 0x%p -> 0x%p (%d)}",
            PSScavenge::should_scavenge(&new_obj) ? "copying" : "tenuring",
            new_obj->klass()->internal_name(),
            (void*) o, (void*) new_obj, new_obj->size());
      }
#endif // PRODUCT

      if (new_obj->is_container()) {
        // The thread that "owns" a container also "owns" all its contained
        // objects. We need to forward all of them to their new locations.
        // Let's consider the memory layout of some container, for example,
        // a structured array.
        // +---------+-------+-------+-------+-------+-----+-------+-------+
        // |Container| RCO_0 | Obj_0 | RCO_1 | Obj_1 | ... |RCO_n-1|Obj_n-1|
        // +---------+-------+-------+-------+-------+-----+-------+-------+
        // Every cell is a single Java object, except for Obj_0 ... Obj_n-1,
        // which may be single Java objects or nested containers having
        // (recursively) the same memory layout. In any case, new_obj_size
        // stores the full size of new_obj container, including (recursively)
        // all its contents. new_obj->size() returns just the container's own
        // size (that is the size of the first cell only). To forward all
        // the contents of new_obj container, we simply need to iterate through
        // all Java objects between ((HeapWord*) o) + new_obj->size() and
        // ((HeapWord*) o) + new_obj_size and forward every object to the
        // corresponding new location. This approach correctly works for
        // the case of nested containers too and corresponds to depth-first
        // traversal of the tree representing container hierarchy for new_obj.
        // For installing forwarding pointers we can use the non MT safe
        // oopDesc::forward_to(), since no other thread can interfere with us.

        size_t size = new_obj->size();
        HeapWord* src = ((HeapWord*) o) + size;
        HeapWord* dst = ((HeapWord*) new_obj) + size;
        HeapWord* src_end = ((HeapWord*) o) + new_obj_size;
        while (src < src_end) {
          ((oop) src)->forward_to((oop) dst);

          // Increment age if the object is still in the young generation
          if (!new_obj_is_tenured) {
            assert(young_space()->contains((oop) dst),
                "object must belong to To space");
            ((oop) dst)->incr_age();
          }

#ifndef PRODUCT
          if (TraceScavenge) {
            ResourceMark rm;
            oop obj = (oop) dst;
            gclog_or_tty->print_cr("{%s %s 0x%p -> 0x%p (%d)}",
                PSScavenge::should_scavenge(&obj) ? "copying" : "tenuring",
                obj->klass()->internal_name(),
                (void*) src, (void*) dst, obj->size());
          }
#endif // PRODUCT

          size = ((oop) dst)->size();
          src += size;
          dst += size;
        }
        assert(src == src_end, "sanity");
      }

      // Firstly, we do the size comparison with new_obj_size, which we
      // already have. Hopefully, only a few objects are larger than
      // _min_array_size_for_chunking, and most of them will be arrays.
      // So, the is_objArray() test would be very infrequent.
      if (new_obj_size > _min_array_size_for_chunking &&
          new_obj->is_objArray() && PSChunkLargeArrays) {
        // We'll chunk it.
        oop* const masked_o = mask_chunked_array_oop(o);
        push_depth(masked_o);
        TASKQUEUE_STATS_ONLY(++_arrays_chunked; ++_masked_pushes);
      } else {
        // We'll just push its contents.
        new_obj->push_contents(this);
      }
    } else {
      // We lost, someone else "owns" this object.
      guarantee(o->is_forwarded(), "object must be forwarded if CAS failed");

      // Here we try to deallocate the space. If it was directly allocated,
      // we cannot deallocate it, so we have to test. If the deallocation
      // fails, we fill the space with a filler object.
      if (new_obj_is_tenured) {
        // TODO: Update object start array
        if (!_old_lab.unallocate_object((HeapWord*) new_obj, new_obj_size)) {
          CollectedHeap::fill_with_object((HeapWord*) new_obj, new_obj_size);
        }
      } else {
        if (!_young_lab.unallocate_object((HeapWord*) new_obj, new_obj_size)) {
          CollectedHeap::fill_with_object((HeapWord*) new_obj, new_obj_size);
        }
      }

      // Don't update this before the unallocation!
      new_obj = o->forwardee();
    }
  } else {
    assert(o->is_forwarded(), "sanity");
    new_obj = o->forwardee();
  }

  if (need_container_copying) {
    // The outermost container of the original object is guaranteed to be
    // forwarded at this point, either by this thread or by another GC worker.
    // The original object itself may be not forwarded yet (if another GC worker
    // is still processing the contained objects of the outermost container),
    // but we do exactly know its new location. Here we just calculate and
    // return it; it is guaranteed to be valid when Scavenge ends.
    new_obj = (oop) (((address) new_obj) + offset_from_outermost_container);
  }

  return new_obj;
}

inline oop PSPromotionManager::allocate_in_young_gen_slow(size_t size) {
  oop obj = NULL;

  if (!_young_gen_is_full) {
    // Do we allocate directly, or flush and fill?
    if (size > (YoungPLABSize / 2)) {
      // Allocate this object directly
      obj = (oop) young_space()->cas_allocate(size);
    } else {
      // Flush and fill
      _young_lab.flush();

      HeapWord* lab_base = young_space()->cas_allocate(YoungPLABSize);
      if (lab_base != NULL) {
        _young_lab.initialize(MemRegion(lab_base, YoungPLABSize));

        // Try allocation in the young generation LAB again
        obj = (oop) _young_lab.allocate(size);
      } else {
        _young_gen_is_full = true;
      }
    }
  }

  return obj;
}

inline oop PSPromotionManager::allocate_in_old_gen_slow(size_t size) {
  oop obj = NULL;

  if (!_old_gen_is_full) {
    // Do we allocate directly, or flush and fill?
    if (size > (OldPLABSize / 2)) {
      // Allocate this object directly
      // TODO: Update object start array
      obj = (oop) old_gen()->cas_allocate(size);
    } else {
      // Flush and fill
      _old_lab.flush();

      HeapWord* lab_base = old_gen()->cas_allocate(OldPLABSize);
      if (lab_base != NULL) {
#ifdef ASSERT
        // Delay initialization of the promotion LAB (PLAB)
        // This exposes uninitialized PLABs to card table processing.
        if (GCWorkerDelayMillis > 0) {
          os::sleep(Thread::current(), GCWorkerDelayMillis, false);
        }
#endif // ASSERT
        _old_lab.initialize(MemRegion(lab_base, OldPLABSize));

        // Try allocation in the old generation LAB again
        // TODO: Update object start array
        obj = (oop) _old_lab.allocate(size);
      }
    }
  }

  if (obj == NULL) {
    _old_gen_is_full = true;
  }

  return obj;
}

inline void PSPromotionManager::process_popped_location_depth(StarTask p) {
  if (is_oop_masked(p)) {
    assert(PSChunkLargeArrays, "invariant");
    oop const old = unmask_chunked_array_oop(p);
    process_array_chunk(old);
  } else {
    if (p.is_narrow()) {
      assert(UseCompressedOops, "Error");
      PSScavenge::copy_and_push_safe_barrier<narrowOop, /*promote_immediately=*/false>(this, p);
    } else {
      PSScavenge::copy_and_push_safe_barrier<oop, /*promote_immediately=*/false>(this, p);
    }
  }
}

#if TASKQUEUE_STATS
void PSPromotionManager::record_steal(StarTask& p) {
  if (is_oop_masked(p)) {
    ++_masked_steals;
  }
}
#endif // TASKQUEUE_STATS

#endif // SHARE_VM_GC_IMPLEMENTATION_PARALLELSCAVENGE_PSPROMOTIONMANAGER_INLINE_HPP
