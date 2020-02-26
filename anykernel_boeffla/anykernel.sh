# AnyKernel3 Ramdisk Mod Script
# osm0sis @ xda-developers

## AnyKernel setup
# begin properties
properties() { '
kernel.string=Tuned Kernel by fbs@xda-developers
do.devicecheck=1
do.modules=0
do.systemless=1
do.cleanup=1
do.cleanuponabort=0
device.name1=kltexx
device.name2=kltelra
device.name3=kltetmo
device.name4=kltecan
device.name5=klteatt
device.name6=klteub
device.name7=klteacg
device.name8=klte
device.name9=kltekor
device.name10=klteskt
device.name11=kltektt
device.name12=kltekdi
device.name13=kltedv
device.name14=kltespr
supported.versions=
supported.patchlevels=
'; } # end properties

# shell variables
block=/dev/block/platform/msm_sdcc.1/by-name/boot;
is_slot_device=0;
ramdisk_compression=auto;


## AnyKernel methods (DO NOT CHANGE)
# import patching functions/variables - see for reference
. tools/ak3-core.sh;


## AnyKernel file attributes
# set permissions/ownership for included ramdisk files
#<owner> <group> <dir_mode> <file_mode> <dir>
set_perm_recursive 0 0 755 755 $ramdisk/*;
set_perm_recursive 0 0 750 750 $ramdisk/init* $ramdisk/sbin;

## AnyKernel install
dump_boot;

# begin ramdisk changes

ASD=$(cat /system/build.prop | grep ro.build.version.sdk | cut -d "=" -f 2)
if [ "$ASD" == "24" ] || [ "$ASD" == "25" ]; then
 ui_print "Android 7.0/7.1 detected!";
 touch $ramdisk/nougat;
fi;

mount -o rw,remount /system
mount -o rw,remount /system_root

if [ -d /system_root ]; then
 ui_print "Android 10+ detected! System-On-Root";
 cp -r res/* /system_root/res
 cp sbin/busybox /system_root/sbin
 chmod 755 /system_root/sbin/busybox
 chmod -R 755 /system_root/res/bc
fi;

replace_file /system/etc/init.d/10vnswap 755 10vnswap
replace_file /system/etc/init/init_d.rc 755 init_d.rc
replace_file /system/bin/sysinit 755 sysinit

# end ramdisk changes

write_boot;
## end install

