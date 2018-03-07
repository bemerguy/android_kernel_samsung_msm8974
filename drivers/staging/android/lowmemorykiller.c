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

#if defined(CONFIG_ZSWAP)
extern u64 zswap_pool_pages;
extern atomic_t zswap_stored_pages;
#endif
extern bool displayon;
static int lowmem_shrink(void)
{
	struct task_struct *tsk, *tokill[16], *p;
	struct task_struct *selected = NULL;
	unsigned long rem = 0;
	int i, selected_oom_score;
	short min_score_adj = OOM_SCORE_ADJ_MAX + 1;
	int minfree = 0, oom_score, tki;
	int array_size = ARRAY_SIZE(lowmem_adj);
	int other_free = global_page_state(NR_FREE_PAGES) - totalreserve_pages;
	int other_file = global_page_state(NR_FILE_PAGES) -
						global_page_state(NR_SHMEM) -
						total_swapcache_pages();

        long cache_size, cache_limit, free;
	static unsigned int expire=0, count=0;

	if (other_free < 0) other_free = 0;

	if (lowmem_adj_size < array_size)
		array_size = lowmem_adj_size;
	if (lowmem_minfree_size < array_size)
		array_size = lowmem_minfree_size;
	for (i = array_size-1; i >=0; i--) {
		minfree = lowmem_minfree[i];
		if (other_free < minfree && other_file < minfree) {
			min_score_adj = lowmem_adj[i];
			break;
		}
	}

	if (++expire > 10) { expire=0; count=0; }

	if (i < 0) {
		lowmem_print(3, "i < 0. min_score_adj = %d. We still have %u pages in cache", min_score_adj, other_file);
		return 0;
	}
	else {
           free = other_free * (long)(PAGE_SIZE / 1024);
           cache_size = other_file * (long)(PAGE_SIZE / 1024);
	   lowmem_print(2, "############### LOW MEMORY KILLER: %ldKb less than hiddenapps minimum: %ldKb. And free: %ldKb",
			cache_size, minfree*(long)(PAGE_SIZE / 1024), free);
	}
	rcu_read_lock();

	restart:
	tki = -1;
	selected = NULL;
	selected_oom_score = min_score_adj;

	cache_limit = minfree * (long)(PAGE_SIZE / 1024);

	for_each_process(tsk) {
		oom_score = 0;

		if (tsk->flags & PF_KTHREAD)
			continue;

		if (same_thread_group(tsk, current))
			continue;

		p = find_lock_task_mm(tsk);
		if (!p)
			continue;

		oom_score = p->signal->oom_score_adj;
			task_unlock(p);

                if (oom_score < min_score_adj) {
                                lowmem_print(5,"%s score: %d < min_score_adj: %d", p->comm, oom_score, min_score_adj);
			continue;
		}


		if (selected) {
			if (oom_score < selected_oom_score) {
				lowmem_print(5,"%s score (%d) is lower than %s's (%d)", p->comm, oom_score, selected->comm, selected_oom_score);
				continue;
		}
			else if ((oom_score == selected_oom_score) && (tki < 16)) {
					tki++;
					lowmem_print(4,"### adding %s with score %d to pos %d of tokill", p->comm, oom_score, tki);
					tokill[tki] = p;
			}
			else if (oom_score > selected_oom_score) {
					tki=0;
					lowmem_print(4,"### oops, adding %s with score %d to pos %d of tokill", p->comm, oom_score, tki);
					tokill[tki] = p;
			}
		}
		selected = p;
		selected_oom_score = oom_score;

	}

	while (tki >= 0) {
		set_tsk_thread_flag(tokill[tki], TIF_MEMDIE);
                send_sig(SIGKILL, tokill[tki], 0);

                lowmem_print(1, "Killing '%s' (%d) on behalf of '%s' (%d) because\n" \
				"   cache %ldkB is below limit %ldkB for oom_score_adj %hd\n" \
                                "   Free memory is %ldkB above reserved\n",
                             tokill[tki]->comm, tokill[tki]->pid,
                             current->comm, current->pid, cache_size, cache_limit, min_score_adj, free);
                rem++;
                lowmem_lmkcount++;
		tki--;
	}

        if (!rem && i>0) {
           min_score_adj = lowmem_adj[--i];
	   minfree = lowmem_minfree[i];
	   cache_limit = minfree * (long)(PAGE_SIZE / 1024);
           lowmem_print(2, "Nothing to kill? min_score_adj decreased to: %d", min_score_adj);
	   goto restart;
        }

	if (rem) {
	   if (++count > 4) {
		lowmem_print(1, "I think you have too many apps running in background. Please try to disable their auto-start on boot!");
		min_score_adj = lowmem_adj[3];
		minfree = lowmem_minfree[3];
		count=0;
		goto restart;
	   }
	   lowmem_print(1, "Killed %d processes", rem);
	}

	rcu_read_unlock();
	return 0;
}

static void timelylmk(struct work_struct *work)
{
	struct delayed_work *dwork = to_delayed_work(work);

	lowmem_shrink();

	if (displayon)
		queue_delayed_work(system_nrt_wq, dwork, 400);
	else
		queue_delayed_work(system_nrt_wq, dwork, 1600);
}

static int __init lowmem_init(void)
{
	struct delayed_work *dwork;
	dwork = kmalloc(sizeof(*dwork), GFP_KERNEL);
	INIT_DELAYED_WORK_DEFERRABLE(dwork, timelylmk);
	queue_delayed_work(system_nrt_wq, dwork, 20000);

	return 0;
}

static void __exit lowmem_exit(void)
{
}

#ifdef CONFIG_ANDROID_LOW_MEMORY_KILLER_AUTODETECT_OOM_ADJ_VALUES
static short lowmem_oom_adj_to_oom_score_adj(short oom_adj)
{
	if (oom_adj == OOM_ADJUST_MAX)
		return OOM_SCORE_ADJ_MAX;
	else
		return (oom_adj * OOM_SCORE_ADJ_MAX) / -OOM_DISABLE;
}

static void lowmem_autodetect_oom_adj_values(void)
{
	int i;
	short oom_adj;
	short oom_score_adj;
	int array_size = ARRAY_SIZE(lowmem_adj);

	if (lowmem_adj_size < array_size)
		array_size = lowmem_adj_size;

	if (array_size <= 0)
		return;

	oom_adj = lowmem_adj[array_size - 1];
	if (oom_adj > OOM_ADJUST_MAX)
		return;

	oom_score_adj = lowmem_oom_adj_to_oom_score_adj(oom_adj);
	if (oom_score_adj <= OOM_ADJUST_MAX)
		return;

	lowmem_print(1, "lowmem_shrink: convert oom_adj to oom_score_adj:\n");
	for (i = 0; i < array_size; i++) {
		oom_adj = lowmem_adj[i];
		oom_score_adj = lowmem_oom_adj_to_oom_score_adj(oom_adj);
		lowmem_adj[i] = oom_score_adj;
		lowmem_print(1, "oom_adj %d => oom_score_adj %d\n",
			     oom_adj, oom_score_adj);
	}
}

static int lowmem_adj_array_set(const char *val, const struct kernel_param *kp)
{
	int ret;

	ret = param_array_ops.set(val, kp);

	/* HACK: Autodetect oom_adj values in lowmem_adj array */
	lowmem_autodetect_oom_adj_values();

	return ret;
}

static int lowmem_adj_array_get(char *buffer, const struct kernel_param *kp)
{
	return param_array_ops.get(buffer, kp);
}

static void lowmem_adj_array_free(void *arg)
{
	param_array_ops.free(arg);
}

static struct kernel_param_ops lowmem_adj_array_ops = {
	.set = lowmem_adj_array_set,
	.get = lowmem_adj_array_get,
	.free = lowmem_adj_array_free,
};

static const struct kparam_array __param_arr_adj = {
	.max = ARRAY_SIZE(lowmem_adj),
	.num = &lowmem_adj_size,
	.ops = &param_ops_short,
	.elemsize = sizeof(lowmem_adj[0]),
	.elem = lowmem_adj,
};
#endif

/*
 * not really modular, but the easiest way to keep compat with existing
 * bootargs behaviour is to continue using module_param here.
 */
#ifdef CONFIG_ANDROID_LOW_MEMORY_KILLER_AUTODETECT_OOM_ADJ_VALUES
module_param_cb(adj, &lowmem_adj_array_ops,
		    .arr = &__param_arr_adj,
		S_IRUGO | S_IWUSR);
__MODULE_PARM_TYPE(adj, "array of short");
#else
module_param_array_named(adj, lowmem_adj, short, &lowmem_adj_size,
			 S_IRUGO | S_IWUSR);
#endif
module_param_array_named(minfree, lowmem_minfree, uint, &lowmem_minfree_size,
			 S_IRUGO | S_IWUSR);
module_param_named(debug_level, lowmem_debug_level, uint, S_IRUGO | S_IWUSR);
module_param_named(lmkcount, lowmem_lmkcount, uint, S_IRUGO);

module_init(lowmem_init);
module_exit(lowmem_exit);

MODULE_LICENSE("GPL");

