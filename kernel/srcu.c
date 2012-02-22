/*
 * Sleepable Read-Copy Update mechanism for mutual exclusion.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 *
 * Copyright (C) IBM Corporation, 2006
 *
 * Author: Paul McKenney <paulmck@us.ibm.com>
 *
 * For detailed explanation of Read-Copy Update mechanism see -
 * 		Documentation/RCU/ *.txt
 *
 */

#include <linux/export.h>
#include <linux/mutex.h>
#include <linux/percpu.h>
#include <linux/preempt.h>
#include <linux/rcupdate.h>
#include <linux/sched.h>
#include <linux/smp.h>
#include <linux/delay.h>
#include <linux/srcu.h>

static int init_srcu_struct_fields(struct srcu_struct *sp)
{
	sp->completed = 0;
	mutex_init(&sp->mutex);
	sp->per_cpu_ref = alloc_percpu(struct srcu_struct_array);
	return sp->per_cpu_ref ? 0 : -ENOMEM;
}

#ifdef CONFIG_DEBUG_LOCK_ALLOC

int __init_srcu_struct(struct srcu_struct *sp, const char *name,
		       struct lock_class_key *key)
{
	/* Don't re-initialize a lock while it is held. */
	debug_check_no_locks_freed((void *)sp, sizeof(*sp));
	lockdep_init_map(&sp->dep_map, name, key, 0);
	return init_srcu_struct_fields(sp);
}
EXPORT_SYMBOL_GPL(__init_srcu_struct);

#else /* #ifdef CONFIG_DEBUG_LOCK_ALLOC */

/**
 * init_srcu_struct - initialize a sleep-RCU structure
 * @sp: structure to initialize.
 *
 * Must invoke this on a given srcu_struct before passing that srcu_struct
 * to any other function.  Each srcu_struct represents a separate domain
 * of SRCU protection.
 */
int init_srcu_struct(struct srcu_struct *sp)
{
	return init_srcu_struct_fields(sp);
}
EXPORT_SYMBOL_GPL(init_srcu_struct);

#endif /* #else #ifdef CONFIG_DEBUG_LOCK_ALLOC */

/*
 * srcu_readers_active_idx -- returns approximate number of readers
 *	active on the specified rank of per-CPU counters.
 */

static int srcu_readers_active_idx(struct srcu_struct *sp, int idx)
{
	int cpu;
	int sum;

	/*
	 * Note that srcu_readers_active_idx() can incorrectly return
	 * zero even though there is a pre-existing reader throughout.
	 * To see this, suppose that task A is in a very long SRCU
	 * read-side critical section that started on CPU 0, and that
	 * no other reader exists, so that the modulo sum of the counters
	 * is equal to one.  Then suppose that task B starts executing
	 * srcu_readers_active_idx(), summing up to CPU 1, and then that
	 * task C starts reading on CPU 0, so that its increment is not
	 * summed, but finishes reading on CPU 2, so that its decrement
	 * -is- summed.  Then when task B completes its sum, it will
	 * incorrectly get zero, despite the fact that task A has been
	 * in its SRCU read-side critical section the whole time.
	 *
	 * We therefore do a validation step should srcu_readers_active_idx()
	 * return zero.
	 */
	if (srcu_readers_active_idx(sp, idx) != 0)
		return false;

	/*
	 * Since the caller recently flipped ->completed, we can see at
	 * most one increment of each CPU's counter from this point
	 * forward.  The reason for this is that the reader CPU must have
	 * fetched the index before srcu_readers_active_idx checked
	 * that CPU's counter, but not yet incremented its counter.
	 * Its eventual counter increment will follow the read in
	 * srcu_readers_active_idx(), and that increment is immediately
	 * followed by smp_mb() B.  Because smp_mb() D is between
	 * the ->completed flip and srcu_readers_active_idx()'s read,
	 * that CPU's subsequent load of ->completed must see the new
	 * value, and therefore increment the counter in the other rank.
	 */
	smp_mb(); /* A */

	/*
	 * Now, we check the ->snap array that srcu_readers_active_idx()
	 * filled in from the per-CPU counter values. Since
	 * __srcu_read_lock() increments the upper bits of the per-CPU
	 * counter, an increment/decrement pair will change the value
	 * of the counter.  Since there is only one possible increment,
	 * the only way to wrap the counter is to have a huge number of
	 * counter decrements, which requires a huge number of tasks and
	 * huge SRCU read-side critical-section nesting levels, even on
	 * 32-bit systems.
	 *
	 * All of the ways of confusing the readings require that the scan
	 * in srcu_readers_active_idx() see the read-side task's decrement,
	 * but not its increment.  However, between that decrement and
	 * increment are smb_mb() B and C.  Either or both of these pair
	 * with smp_mb() A above to ensure that the scan below will see
	 * the read-side tasks's increment, thus noting a difference in
	 * the counter values between the two passes.
	 *
	 * Therefore, if srcu_readers_active_idx() returned zero, and
	 * none of the counters changed, we know that the zero was the
	 * correct sum.
	 *
	 * Of course, it is possible that a task might be delayed
	 * for a very long time in __srcu_read_lock() after fetching
	 * the index but before incrementing its counter.  This
	 * possibility will be dealt with in __synchronize_srcu().
	 */
	for_each_possible_cpu(cpu)
		sum += per_cpu_ptr(sp->per_cpu_ref, cpu)->c[idx];
	return sum;
}

/**
 * srcu_readers_active - returns approximate number of readers.
 * @sp: which srcu_struct to count active readers (holding srcu_read_lock).
 *
 * Note that this is not an atomic primitive, and can therefore suffer
 * severe errors when invoked on an active srcu_struct.  That said, it
 * can be useful as an error check at cleanup time.
 */
static int srcu_readers_active(struct srcu_struct *sp)
{
	return srcu_readers_active_idx(sp, 0) + srcu_readers_active_idx(sp, 1);
}

/**
 * cleanup_srcu_struct - deconstruct a sleep-RCU structure
 * @sp: structure to clean up.
 *
 * Must invoke this after you are finished using a given srcu_struct that
 * was initialized via init_srcu_struct(), else you leak memory.
 */
void cleanup_srcu_struct(struct srcu_struct *sp)
{
	int sum;

	sum = srcu_readers_active(sp);
	WARN_ON(sum);  /* Leakage unless caller handles error. */
	if (sum != 0)
		return;
	free_percpu(sp->per_cpu_ref);
	sp->per_cpu_ref = NULL;
}
EXPORT_SYMBOL_GPL(cleanup_srcu_struct);

/*
 * Counts the new reader in the appropriate per-CPU element of the
 * srcu_struct.  Must be called from process context.
 * Returns an index that must be passed to the matching srcu_read_unlock().
 */
int __srcu_read_lock(struct srcu_struct *sp)
{
	int idx;

	preempt_disable();
	idx = sp->completed & 0x1;
	barrier();  /* ensure compiler looks -once- at sp->completed. */
	per_cpu_ptr(sp->per_cpu_ref, smp_processor_id())->c[idx]++;
	srcu_barrier();  /* ensure compiler won't misorder critical section. */
	preempt_enable();
	return idx;
}
EXPORT_SYMBOL_GPL(__srcu_read_lock);

/*
 * Removes the count for the old reader from the appropriate per-CPU
 * element of the srcu_struct.  Note that this may well be a different
 * CPU than that which was incremented by the corresponding srcu_read_lock().
 * Must be called from process context.
 */
void __srcu_read_unlock(struct srcu_struct *sp, int idx)
{
	preempt_disable();
<<<<<<< HEAD
	srcu_barrier();  /* ensure compiler won't misorder critical section. */
	per_cpu_ptr(sp->per_cpu_ref, smp_processor_id())->c[idx]--;
=======
	smp_mb(); /* C */  /* Avoid leaking the critical section. */
	ACCESS_ONCE(this_cpu_ptr(sp->per_cpu_ref)->c[idx]) -= 1;
>>>>>>> 8c8e55638ac... rcu: Increment upper bit only for srcu_read_lock()
	preempt_enable();
}
EXPORT_SYMBOL_GPL(__srcu_read_unlock);

/*
 * We use an adaptive strategy for synchronize_srcu() and especially for
 * synchronize_srcu_expedited().  We spin for a fixed time period
 * (defined below) to allow SRCU readers to exit their read-side critical
 * sections.  If there are still some readers after 10 microseconds,
 * we repeatedly block for 1-millisecond time periods.  This approach
 * has done well in testing, so there is no need for a config parameter.
 */
#define SYNCHRONIZE_SRCU_READER_DELAY 10

/*
 * Helper function for synchronize_srcu() and synchronize_srcu_expedited().
 */
static void __synchronize_srcu(struct srcu_struct *sp, void (*sync_func)(void))
{
	int idx = 0;

	rcu_lockdep_assert(!lock_is_held(&sp->dep_map) &&
			   !lock_is_held(&rcu_bh_lock_map) &&
			   !lock_is_held(&rcu_lock_map) &&
			   !lock_is_held(&rcu_sched_lock_map),
			   "Illegal synchronize_srcu() in same-type SRCU (or RCU) read-side critical section");

	mutex_lock(&sp->mutex);

	/*
	 * If there were no helpers, then we need to do two flips of
	 * the index.  The first flip is required if there are any
	 * outstanding SRCU readers even if there are no new readers
	 * running concurrently with the first counter flip.
	 *
	 * So it is now safe to do the flip.
	 */

	idx = sp->completed & 0x1;
	sp->completed++;

	sync_func();  /* Force memory barrier on all CPUs. */

	/*
	 * At this point, because of the preceding synchronize_sched(),
	 * all srcu_read_lock() calls using the old counters have completed.
	 * Their corresponding critical sections might well be still
	 * executing, but the srcu_read_lock() primitives themselves
	 * will have finished executing.  We initially give readers
	 * an arbitrarily chosen 10 microseconds to get out of their
	 * SRCU read-side critical sections, then loop waiting 1/HZ
	 * seconds per iteration.  The 10-microsecond value has done
	 * very well in testing.
	 */

	if (srcu_readers_active_idx(sp, idx))
		udelay(SYNCHRONIZE_SRCU_READER_DELAY);
	while (srcu_readers_active_idx(sp, idx))
		schedule_timeout_interruptible(1);

	sync_func();  /* Force memory barrier on all CPUs. */

	/*
	 * The preceding synchronize_sched() forces all srcu_read_unlock()
	 * primitives that were executing concurrently with the preceding
	 * for_each_possible_cpu() loop to have completed by this point.
	 * More importantly, it also forces the corresponding SRCU read-side
	 * critical sections to have also completed, and the corresponding
	 * references to SRCU-protected data items to be dropped.
	 *
	 * Note:
	 *
	 *	Despite what you might think at first glance, the
	 *	preceding synchronize_sched() -must- be within the
	 *	critical section ended by the following mutex_unlock().
	 *	Otherwise, a task taking the early exit can race
	 *	with a srcu_read_unlock(), which might have executed
	 *	just before the preceding srcu_readers_active() check,
	 *	and whose CPU might have reordered the srcu_read_unlock()
	 *	with the preceding critical section.  In this case, there
	 *	is nothing preventing the synchronize_sched() task that is
	 *	taking the early exit from freeing a data structure that
	 *	is still being referenced (out of order) by the task
	 *	doing the srcu_read_unlock().
	 *
	 *	Alternatively, the comparison with "2" on the early exit
	 *	could be changed to "3", but this increases synchronize_srcu()
	 *	latency for bulk loads.  So the current code is preferred.
	 */

	mutex_unlock(&sp->mutex);
}

/**
 * synchronize_srcu - wait for prior SRCU read-side critical-section completion
 * @sp: srcu_struct with which to synchronize.
 *
 * Flip the completed counter, and wait for the old count to drain to zero.
 * As with classic RCU, the updater must use some separate means of
 * synchronizing concurrent updates.  Can block; must be called from
 * process context.
 *
 * Note that it is illegal to call synchronize_srcu() from the corresponding
 * SRCU read-side critical section; doing so will result in deadlock.
 * However, it is perfectly legal to call synchronize_srcu() on one
 * srcu_struct from some other srcu_struct's read-side critical section.
 */
void synchronize_srcu(struct srcu_struct *sp)
{
	__synchronize_srcu(sp, synchronize_sched);
}
EXPORT_SYMBOL_GPL(synchronize_srcu);

/**
 * synchronize_srcu_expedited - Brute-force SRCU grace period
 * @sp: srcu_struct with which to synchronize.
 *
 * Wait for an SRCU grace period to elapse, but use a "big hammer"
 * approach to force the grace period to end quickly.  This consumes
 * significant time on all CPUs and is unfriendly to real-time workloads,
 * so is thus not recommended for any sort of common-case code.  In fact,
 * if you are using synchronize_srcu_expedited() in a loop, please
 * restructure your code to batch your updates, and then use a single
 * synchronize_srcu() instead.
 *
 * Note that it is illegal to call this function while holding any lock
 * that is acquired by a CPU-hotplug notifier.  And yes, it is also illegal
 * to call this function from a CPU-hotplug notifier.  Failing to observe
 * these restriction will result in deadlock.  It is also illegal to call
 * synchronize_srcu_expedited() from the corresponding SRCU read-side
 * critical section; doing so will result in deadlock.  However, it is
 * perfectly legal to call synchronize_srcu_expedited() on one srcu_struct
 * from some other srcu_struct's read-side critical section, as long as
 * the resulting graph of srcu_structs is acyclic.
 */
void synchronize_srcu_expedited(struct srcu_struct *sp)
{
	__synchronize_srcu(sp, synchronize_sched_expedited);
}
EXPORT_SYMBOL_GPL(synchronize_srcu_expedited);

/**
 * srcu_batches_completed - return batches completed.
 * @sp: srcu_struct on which to report batch completion.
 *
 * Report the number of batches, correlated with, but not necessarily
 * precisely the same as, the number of grace periods that have elapsed.
 */

long srcu_batches_completed(struct srcu_struct *sp)
{
	return sp->completed;
}
EXPORT_SYMBOL_GPL(srcu_batches_completed);
