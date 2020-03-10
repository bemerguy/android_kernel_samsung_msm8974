#include <linux/workqueue.h>
#include <linux/cpu.h>
#include <linux/sched.h>
#include <linux/module.h>
#include <linux/rq_stats.h>
#include <linux/cpufreq.h>
#include <linux/delay.h>
//#include <stddef.h>
#include "../vnswap/vnswap.h"

static void tunedinit(struct work_struct *work);
static struct workqueue_struct *tunedinit_wq;
static struct delayed_work tunedinit_work;
extern int selinux_enforcing;

static int tunedinittimer(void)
{
   tunedinit_wq = alloc_workqueue("tunedstart", WQ_HIGHPRI, 1);

   if (!tunedinit_wq)
      return -ENOMEM;

   INIT_DELAYED_WORK(&tunedinit_work, tunedinit);

   queue_delayed_work_on(0, tunedinit_wq, &tunedinit_work, 20*HZ);

   return 0;
}

static void tunedinit(struct work_struct *work)
{
   char *argv[4];
   char *envp[5];
   int ret;

   envp[0] = "HOME=/";
   envp[1] = "USER=root";
   envp[2] = "PATH=/sbin:/system/sbin:/system/bin:/system/xbin";
   envp[3] = "PWD=/";
   envp[4] = NULL;

   argv[0] = "/system/bin/toybox";
   argv[1] = "stop";
   argv[2] = "mpdecision";
   argv[3] = NULL;

   selinux_enforcing = 0;

   ret = call_usermodehelper(argv[0], argv, envp, UMH_WAIT_PROC);
   printk("Tuned init %s %s ret %d\n", argv[0], argv[1], ret);

   argv[0] = "/system/sbin/toybox";
   ret = call_usermodehelper(argv[0], argv, envp, UMH_WAIT_PROC);
   printk("Tuned init %s %s ret %d\n", argv[0], argv[1], ret);

   argv[0] = "/system/bin/toybox";
   argv[2] = "thermal-engine";
   ret = call_usermodehelper(argv[0], argv, envp, UMH_WAIT_PROC);
   argv[0] = "/system/sbin/toybox";
   ret = call_usermodehelper(argv[0], argv, envp, UMH_WAIT_PROC);

   vnswap_init_disksize(536870912);

   argv[0] = "/sbin/busybox";
   argv[1] = "mkswap";
   argv[2] = "/dev/block/vnswap0";
   ret = call_usermodehelper(argv[0], argv, envp, UMH_WAIT_PROC);
   printk("Tuned init %s %s ret %d\n", argv[0], argv[1], ret);

   argv[0] = "/sbin/.magisk/busybox/busybox";
   ret = call_usermodehelper(argv[0], argv, envp, UMH_WAIT_PROC);
   printk("Tuned init %s %s ret %d\n", argv[0], argv[1], ret);

   argv[0] = "/sbin/busybox";
   argv[1] = "swapon";
   argv[2] = "/dev/block/vnswap0";
   ret = call_usermodehelper(argv[0], argv, envp, UMH_WAIT_PROC);
   printk("Tuned init %s %s ret %d\n", argv[0], argv[1], ret);

   argv[0] = "/sbin/.magisk/busybox/busybox";
   argv[1] = "swapon";
   argv[2] = "/dev/block/vnswap0";
   ret = call_usermodehelper(argv[0], argv, envp, UMH_WAIT_PROC);
   printk("Tuned init %s %s ret %d\n", argv[0], argv[1], ret);

//   selinux_enforcing = 1;
}

late_initcall(tunedinittimer);
