/* drivers/misc/lowmemorykiller.c
 *
 * The lowmemorykiller driver lets user-space specify a set of memory thresholds
 * where processes with a range of oom_score_adj values will get killed. Specify
 * the minimum oom_score_adj values in
 * /sys/module/lowmemorykiller/parameters/adj and the number of free pages in
 * /sys/module/lowmemorykiller/parameters/minfree. Both files take a comma
 * separated list of numbers in ascending order.
 *
 * For example, write "0,8" to /sys/module/lowmemorykiller/parameters/adj and
 * "1024,4096" to /sys/module/lowmemorykiller/parameters/minfree to kill
 * processes with a oom_score_adj value of 8 or higher when the free memory
 * drops below 4096 pages and kill processes with a oom_score_adj value of 0 or
 * higher when the free memory drops below 1024 pages.
 *
 * The driver considers memory used for caches to be free, but if a large
 * percentage of the cached memory is locked this can be very inaccurate
 * and processes may not get killed until the normal oom killer is triggered.
 *
 * Copyright (C) 2007-2008 Google, Inc.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

/* rework by fbs (heiler.bemerguy@gmail.com) 2018/2019
 * let's do it in a timely fashion with freezable timer instead of using kernel
 * shrink functions, and killing up to 16 process at a time
*/

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/module.h>
#include <linux/init.h>
#include <linux/moduleparam.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/oom.h>
#include <linux/sched.h>
#include <linux/swap.h>
#include <linux/rcupdate.h>
#include <linux/profile.h>
#include <linux/notifier.h>
#include <linux/delay.h>

static uint32_t lowmem_debug_level = 1;
static short lowmem_adj[6] = {
	0,
	1,
	6,
	12,
};
static int lowmem_adj_size = 4;
static int lowmem_minfree[6] = {
	3 * 512,	/* 6MB */
	2 * 1024,	/* 8MB */
	4 * 1024,	/* 16MB */
	16 * 1024,	/* 64MB */
};
static int lowmem_minfree_size = 4;
static uint32_t lowmem_lmkcount = 0;

#define lowmem_print(level, x...)			\
	do {						\
		if (lowmem_debug_level >= (level))	\
			pr_info(x);			\
	} while (0)

#ifdef CONFIG_TUNED_PLUG
extern bool displayon;
#endif

static int lowmem_shrink(void)
{
	struct task_struct *tsk, *tokill[16], *p;
	unsigned long rem = 0;
	int i;
	short min_score_adj = OOM_SCORE_ADJ_MAX + 1;
	int minfree = 0, oom_score, tki = 0;
	int array_size = ARRAY_SIZE(lowmem_adj);
	int free = global_page_state(NR_FREE_PAGES) + global_page_state(NR_FILE_PAGES) - 25;

	static unsigned int expire=0, count=0;

	if (lowmem_adj_size < array_size)
		array_size = lowmem_adj_size;
	if (lowmem_minfree_size < array_size)
		array_size = lowmem_minfree_size;

	for (i = 0; i < array_size; i++) {
		minfree = lowmem_minfree[i];
		if (free < minfree) {
			min_score_adj = lowmem_adj[i];
			break;
		}
	}

	if (++expire > 20) { expire=0; count=0; }

	if (min_score_adj == OOM_SCORE_ADJ_MAX + 1) {
		lowmem_print(3, "min_score_adj = %d. We still have %ldKb free\n",
			min_score_adj, free * (long)(PAGE_SIZE / 1024));
		return 0;
	}
	else {
		lowmem_print(1, "############### LOW MEMORY KILLER: %ldKb less than adj %d's minimum: %ldKb.\n",
			free * (long)(PAGE_SIZE / 1024), min_score_adj, minfree*(long)(PAGE_SIZE / 1024));
	}
	rcu_read_lock();

	restart:
	tki = -1;

	for_each_process(tsk) {
		oom_score = 0;

		if (tsk->flags & PF_KTHREAD)
			continue;

                p = find_lock_task_mm(tsk);
                if (!p)
                        continue;

                oom_score = p->signal->oom_score_adj;

		task_unlock(p);
                if (oom_score < min_score_adj)
			continue;


		if ((oom_score >= min_score_adj) && (tki < 16)) {
				tki++;
				tokill[tki] = p;
		}
	}

	if (tki!=-1) lowmem_print(1, "NOW start killing %d process(es)", tki+1);

	while (tki >=0) {
		task_lock(tokill[tki]);
		set_tsk_thread_flag(tokill[tki], TIF_MEMDIE);
                send_sig(SIGKILL, tokill[tki], 0);

                lowmem_print(1, "Killing '%s' (%d) on behalf of '%s' (%d)", tokill[tki]->comm, tokill[tki]->pid, current->comm, current->pid);

		task_unlock(tokill[tki]);

                rem++;
                lowmem_lmkcount++;
		tki--;
	}

        if (!rem && i>0) {
		i--;
		min_score_adj = lowmem_adj[i];
		minfree = lowmem_minfree[i];
		lowmem_print(2, "Nothing to kill? min_score_adj decreased to: %d", min_score_adj);
		goto restart;
        }

	if (rem) {
	   if (++count > 4) {
		lowmem_print(1, "I think you have too many apps running in background. Please try to disable their auto-start on boot!");
		min_score_adj = lowmem_adj[2];
		minfree = lowmem_minfree[2];
		count=0;
		goto restart;
	   }
	}

	rcu_read_unlock();
	return 0;
}

static void timelylmk(struct work_struct *work)
{
	struct delayed_work *dwork = to_delayed_work(work);

	lowmem_shrink();

#ifdef CONFIG_TUNED_PLUG
	if (displayon)
		queue_delayed_work(system_nrt_freezable_wq, dwork, HZ*2);
	else
#endif
		queue_delayed_work(system_nrt_freezable_wq, dwork, HZ*5);
}

static int __init lowmem_init(void)
{
	struct delayed_work *dwork;
	dwork = kmalloc(sizeof(*dwork), GFP_KERNEL);
	INIT_DELAYED_WORK_DEFERRABLE(dwork, timelylmk);
	queue_delayed_work(system_nrt_freezable_wq, dwork, 30000);

	return 0;
}

static void __exit lowmem_exit(void)
{
}

module_param_array_named(adj, lowmem_adj, short, &lowmem_adj_size,
			 S_IRUGO | S_IWUSR);

module_param_array_named(minfree, lowmem_minfree, uint, &lowmem_minfree_size,
			 S_IRUGO | S_IWUSR);
module_param_named(debug_level, lowmem_debug_level, uint, S_IRUGO | S_IWUSR);
module_param_named(lmkcount, lowmem_lmkcount, uint, S_IRUGO);

module_init(lowmem_init);
module_exit(lowmem_exit);

MODULE_LICENSE("GPL");
