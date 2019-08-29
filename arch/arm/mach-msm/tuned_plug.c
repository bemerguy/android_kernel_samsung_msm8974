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

/* tuned hotplugger by fbs (heiler.bemerguy@gmail.com) */

#include <linux/workqueue.h>
#include <linux/cpu.h>
#include <linux/sched.h>
#include <linux/module.h>
#include <linux/rq_stats.h>
#include <linux/cpufreq.h>
#include <linux/delay.h>
#include <linux/lcd_notify.h>

static struct notifier_block lcd_notif;
static struct workqueue_struct *tunedplug_wq;
static struct delayed_work tunedplug_work;

static unsigned int tunedplug_active = 1;
module_param(tunedplug_active, uint, 0644);

#define DEF_SAMPLING 10*HZ/1000   //10ms
#define MAX_SAMPLING 1000*HZ/1000 //1000ms

/* up threshold. lower means more delay */
static int u[] = { -1, -17, -40 };

/* down threshold. higher means more delay */
static int d[] = { 170, 100, 40 };


static unsigned long sampling_time = DEF_SAMPLING;
bool displayon = true;

static int down[NR_CPUS-1];
/* 123 are cpu cores. */

static void inline down_one(void){
        unsigned int i;
        for (i = NR_CPUS-1; i > 0; i--) {
                if (cpu_online(i)) {
                        if (down[i] > d[i-1]) {
                                cpu_down(i);
                                pr_info("tunedplug: DOWN cpu %d. (%d > %d) sampling: %lu\n",
					i, down[i], d[i-1], sampling_time);
                                down[i]=0;
                        }
                        else down[i]++;
                        return;
                }
        }
}
static void inline up_one(void){
        unsigned int i;
        for (i = 1; i < NR_CPUS; i++) {
                if (!cpu_online(i)) {
                        if (down[i] < u[i-1]) {
                                struct cpufreq_policy policy, *p = &policy;

                                pr_info("tunedplug: UP cpu %d. (%d < %d) sampling: %lu\n",
					i, down[i], u[i-1], sampling_time);

                                cpu_up(i);

                                if (cpufreq_get_policy(&policy, i) != 0)
                                        pr_info("tunedplug: no policy for cpu %d ?", i);
                                else
                                        __cpufreq_driver_target(p, p->max, CPUFREQ_RELATION_H);

                                down[i]=-60;
                        }
                        else down[i]--;
                        return;
                }
        }
}

static void tunedplug_work_fn(struct work_struct *work)
{
	unsigned int i, status[3] = { 0 };
	struct cpufreq_policy policy;

	queue_delayed_work_on(0, tunedplug_wq, &tunedplug_work, sampling_time);

        if (!tunedplug_active)
                return;

	if (!displayon && (sampling_time < MAX_SAMPLING))
		sampling_time++;

#define TMAXFREQ status[0]
#define TLOWFREQ status[1]
#define TONLINE status[2]

	for_each_possible_cpu(i) {
		if (cpu_online(i)) TONLINE++;
		if (i >= NR_CPUS-1 || (cpufreq_get_policy(&policy, i) != 0))
			continue;
		if (policy.cur >= policy.max) TMAXFREQ++;
		else if (policy.cur <= policy.min) TLOWFREQ++;
	}

//	pr_info("ON=%d. LOW=%d. MAX=%d.\n", TONLINE, TLOWFREQ, TMAXFREQ);

	if (TMAXFREQ == TONLINE) up_one();
	else if (TLOWFREQ) down_one();

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
	                sampling_time = DEF_SAMPLING;
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

static int __init tuned_plug_init(void)
{

	tunedplug_wq = alloc_workqueue("tunedplugv2", WQ_HIGHPRI|WQ_FREEZABLE, 1);
        if (!tunedplug_wq)
                return -ENOMEM;

        INIT_DELAYED_WORK(&tunedplug_work, tunedplug_work_fn);

	sampling_time = DEF_SAMPLING;

        queue_delayed_work_on(0, tunedplug_wq, &tunedplug_work, 5000);

	initnotifier();

	return 0;
}

MODULE_AUTHOR("Heiler Bemerguy <heiler.bemerguy@gmail.com>");
MODULE_DESCRIPTION("'tuned_plug' - A simple cpu hotplug driver");
MODULE_LICENSE("GPL");

late_initcall(tuned_plug_init);
