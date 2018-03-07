/*
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

#include <linux/workqueue.h>
#include <linux/cpu.h>
#include <linux/sched.h>
#include <linux/module.h>
#include <linux/rq_stats.h>
#include <linux/cpufreq.h>
#include <linux/delay.h>
#include <linux/lcd_notify.h>

static struct notifier_block lcd_notif;
static struct delayed_work tuned_plug_work;
static struct workqueue_struct *tunedplug_wq;

static unsigned int tuned_plug_active = 1;
module_param(tuned_plug_active, uint, 0644);

static unsigned int sampling_time = 7;

bool displayon = true;
static unsigned int down[NR_CPUS-1];

static void inline down_one(void){
	unsigned int i;
	for (i = NR_CPUS; i > 0; --i) {
		if (cpu_online(i)) {
			if (down[i] > 30) {
				cpu_down(i);
				down[i]=0;
				pr_info("tunedplug: DOWN cpu %d. sampling: %ld", i, sampling_time);
			}
			else down[i]++;
			msleep(100);
			return;
		}
	}
}
static void inline up_one(void){
        unsigned int i;
        for (i = 1; i < NR_CPUS; i++) {
                if (!cpu_online(i)) {
			cpu_up(i);
			down[i]=0;
			pr_info("tunedplug: UP cpu %d", i);
                        return;
                }
        }
}
static void __cpuinit tuned_plug_work_fn(struct work_struct *work)
{
	unsigned int nr_cpus, i;
	struct cpufreq_policy policy;

        queue_delayed_work_on(0, tunedplug_wq, &tuned_plug_work, sampling_time);

        if (!tuned_plug_active)
                return;

	if (!displayon && sampling_time < 50)
		sampling_time++;

        nr_cpus = num_online_cpus();

	/* if any cpu is on its limit, turn on the next one and quit.
	   This is a shortcut. Returning here will prevent unecessary computing
	*/
	if (nr_cpus < NR_CPUS) {
		for_each_online_cpu(i) {
			if (cpufreq_get_policy(&policy, i) != 0)
				continue;
			if (i < NR_CPUS-1 && (policy.cur >= policy.max))
				up_one();
		}
	}

	if (nr_cpus > 1) {
                for_each_online_cpu(i) {
                        if (!i || (cpufreq_get_policy(&policy, i) != 0))
                                continue;
                        if (policy.cur <= policy.min)
				down_one();
                }
	}

}
static int lcd_notifier_callback(struct notifier_block *this,
				unsigned long event, void *data)
{
        switch (event)
        {
                case LCD_EVENT_OFF_END:
			displayon = false;
                        break;

                case LCD_EVENT_ON_START:
			displayon = true;
	                sampling_time = 7;
                        break;

                default:
                        break;
        }
        return 0;
}

static void initnotifier(void)
{
        lcd_notif.notifier_call = lcd_notifier_callback;
        if (lcd_register_client(&lcd_notif) != 0)
                pr_err("%s: Failed to register lcd callback\n", __func__);

}

int __init tuned_plug_init(void)
{

	tunedplug_wq = alloc_workqueue("tunedplug", WQ_HIGHPRI, 0);

	INIT_DELAYED_WORK(&tuned_plug_work, tuned_plug_work_fn);

	sampling_time = 7;

	queue_delayed_work_on(0, tunedplug_wq, &tuned_plug_work, msecs_to_jiffies(10000));

	initnotifier();

	return 0;
}

MODULE_AUTHOR("Heiler Bemerguy <heiler.bemerguy@gmail.com>");
MODULE_DESCRIPTION("'tuned_plug' - A simple cpu hotplug driver");
MODULE_LICENSE("GPL");

late_initcall(tuned_plug_init);
