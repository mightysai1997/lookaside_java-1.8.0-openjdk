/*
 * Copyright (c) 2018, Red Hat, Inc. and/or its affiliates.
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

#include "precompiled.hpp"

#include "gc_implementation/shenandoah/shenandoahPacer.hpp"
#include "gc_implementation/shenandoah/shenandoahHeap.hpp"
#include "gc_implementation/shenandoah/shenandoahHeap.inline.hpp"
#include "gc_implementation/shenandoah/shenandoahFreeSet.hpp"

/*
 * In normal concurrent cycle, we have to pace the application to let GC finish.
 *
 * Here, we do not know how large would be the collection set, and what are the
 * relative performances of the each stage in the concurrent cycle, and so we have to
 * make some assumptions.
 *
 * We assume, for pessimistic reasons, that the entire heap is full of alive objects,
 * and it will be evacuated fully. Therefore, we count live objects visited by all three
 * stages against the heap used at the beginning of the collection. That means if there
 * are dead objects, they would not be accounted for in this budget, and that would mean
 * allocation would be pacified excessively. But that *also* means the collection cycle
 * would finish earlier than pacer expects.
 *
 * The allocatable space when GC is running is "free" at the start of cycle, but the
 * accounted budget is based on "used". So, we need to adjust the tax knowing that.
 * Also, since we effectively count the used space three times (mark, evac, update-refs),
 * we need to multiply the tax by 3. Example: for 10 MB free and 90 MB used, GC would
 * come back with 3*90 MB budget, and thus for each 1 MB of allocation, we have to pay
 * 3*90 / 10 MBs. In the end, we would pay back the entire budget.
 */

void ShenandoahPacer::setup_for_mark() {
  assert(ShenandoahPacing, "Only be here when pacing is enabled");

  size_t used = _heap->used();
  size_t free = _heap->free_set()->available();

  size_t non_taxable = free * ShenandoahPacingCycleSlack / 100;
  size_t taxable = free - non_taxable;

  double tax = 1.0 * used / taxable; // base tax for available free space
  tax *= 3;                          // mark is phase 1 of 3, claim 1/3 of free for it
  tax = MAX2<double>(1, tax);        // never allocate more than GC collects during the cycle
  tax *= 1.1;                        // additional surcharge to help unclutter heap

  restart_with(non_taxable, tax);

  log_info(gc, ergo)("Pacer for Mark. Used: " SIZE_FORMAT "M, Free: " SIZE_FORMAT
                     "M, Non-Taxable: " SIZE_FORMAT "M, Alloc Tax Rate: %.1fx",
                     used / M, free / M, non_taxable / M, tax);
}

void ShenandoahPacer::setup_for_evac() {
  assert(ShenandoahPacing, "Only be here when pacing is enabled");

  size_t cset = _heap->collection_set()->live_data();
  size_t free = _heap->free_set()->available();

  size_t non_taxable = free * ShenandoahPacingCycleSlack / 100;
  size_t taxable = free - non_taxable;

  double tax = 1.0 * cset / taxable; // base tax for available free space
  tax *= 2;                          // evac is phase 2 of 3, claim 1/2 of remaining free
  tax = MAX2<double>(1, tax);        // never allocate more than GC collects during the cycle
  tax *= 1.1;                        // additional surcharge to help unclutter heap

  restart_with(non_taxable, tax);

  log_info(gc, ergo)("Pacer for Evacuation. CSet: " SIZE_FORMAT "M, Free: " SIZE_FORMAT
                     "M, Non-Taxable: " SIZE_FORMAT "M, Alloc Tax Rate: %.1fx",
                     cset / M, free / M, non_taxable / M, tax);
}

void ShenandoahPacer::setup_for_updaterefs() {
  assert(ShenandoahPacing, "Only be here when pacing is enabled");

  size_t used = _heap->used();
  size_t free = _heap->free_set()->available();

  size_t non_taxable = free * ShenandoahPacingCycleSlack / 100;
  size_t taxable = free - non_taxable;

  double tax = 1.0 * used / taxable; // base tax for available free space
  tax *= 1;                          // update-refs is phase 3 of 3, claim the remaining free
  tax = MAX2<double>(1, tax);        // never allocate more than GC collects during the cycle
  tax *= 1.1;                        // additional surcharge to help unclutter heap

  restart_with(non_taxable, tax);

  log_info(gc, ergo)("Pacer for Update-Refs. Used: " SIZE_FORMAT "M, Free: " SIZE_FORMAT
                     "M, Non-Taxable: " SIZE_FORMAT "M, Alloc Tax Rate: %.1fx",
                     used / M, free / M, non_taxable / M, tax);
}

/*
 * In idle phase, we have to pace the application to let control thread react with GC start.
 *
 * Here, we have rendezvous with concurrent thread that adds up the budget as it acknowledges
 * it had seen recent allocations. It will naturally pace the allocations if control thread is
 * not catching up. To bootstrap this feedback cycle, we need to start with some initial budget
 * for applications to allocate at.
 */

void ShenandoahPacer::setup_for_idle() {
  assert(ShenandoahPacing, "Only be here when pacing is enabled");

  size_t initial = _heap->capacity() * ShenandoahPacingIdleSlack / 100;
  double tax = 1;

  restart_with(initial, tax);

  log_info(gc, ergo)("Pacer for Idle. Initial: " SIZE_FORMAT "M, Alloc Tax Rate: %.1fx",
                     initial / M, tax);
}

void ShenandoahPacer::restart_with(jlong non_taxable_bytes, jdouble tax_rate) {
  STATIC_ASSERT(sizeof(size_t) <= sizeof(intptr_t));
  intptr_t initial = (size_t)(non_taxable_bytes * tax_rate) >> LogHeapWordSize;
  intptr_t cur;
  do {
    cur = OrderAccess::load_acquire(&_budget);
  } while (Atomic::cmpxchg(initial, &_budget, cur) != cur);
  OrderAccess::release_store(&_tax_rate, tax_rate);
}

bool ShenandoahPacer::claim_for_alloc(size_t words, bool force) {
  assert(ShenandoahPacing, "Only be here when pacing is enabled");

  intptr_t tax = MAX2<intptr_t>(1, words * OrderAccess::load_acquire(&_tax_rate));

  intptr_t cur = 0;
  intptr_t new_val = 0;
  do {
    cur = OrderAccess::load_acquire(&_budget);
    if (cur < tax) {
      // Progress depleted, alas.
      return false;
    }
    new_val = cur - tax;
  } while (Atomic::cmpxchg(new_val, &_budget, cur) != cur);
  return true;
}

void ShenandoahPacer::pace_for_alloc(size_t words) {
  assert(ShenandoahPacing, "Only be here when pacing is enabled");

  // Fast path: try to allocate right away
  if (claim_for_alloc(words, false)) {
    return;
  }

  size_t max_wait_ms = ShenandoahPacingMaxDelay;
  double start = os::elapsedTime();

  while (true) {
    // We could instead assist GC, but this would suffice for now.
    // This code should also participate in safepointing.
    os::sleep(Thread::current(), 1, true);

    double end = os::elapsedTime();
    size_t ms = (size_t)((end - start) * 1000);
    if (ms > max_wait_ms) {
      // Spent local time budget to wait for enough GC progress.
      // Breaking out and allocating anyway, which may mean we outpace GC,
      // and start Degenerated GC cycle.
      _delays.add(ms);

      // Forcefully claim the budget: it may go negative at this point, and
      // GC should replenish for this and subsequent allocations
      claim_for_alloc(words, true);
      break;
    }

    if (claim_for_alloc(words, false)) {
      // Acquired enough permit, nice. Can allocate now.
      _delays.add(ms);
      break;
    }
  }
}

void ShenandoahPacer::print_on(outputStream* out) const {
  out->print_cr("ALLOCATION PACING:");
  out->cr();

  out->print_cr("Max pacing delay is set for " UINTX_FORMAT " ms.", ShenandoahPacingMaxDelay);
  out->cr();

  out->print_cr("Higher delay would prevent application outpacing the GC, but it will hide the GC latencies");
  out->print_cr("from the STW pause times. Pacing affects the individual threads, and so it would also be");
  out->print_cr("invisible to the usual profiling tools, but would add up to end-to-end application latency.");
  out->print_cr("Raise max pacing delay with care.");
  out->cr();

  out->print_cr("Actual pacing delays histogram:");
  out->cr();

  out->print_cr("%10s - %10s %12s", "From", "To", "Count");
  for (int c = _delays.min_level(); c <= _delays.max_level(); c++) {
    out->print("%7d ms - %7d ms:", (c == 0) ? 0 : 1 << (c - 1), 1 << c);
    out->print_cr(SIZE_FORMAT_W(12), _delays.level(c));
  }
  out->cr();
}