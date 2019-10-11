# AnyKernel2 Script
#
# Original and credits: osm0sis @ xda-developers
#
# Modified by sunilpaulmathew @ xda-developers.com

############### AnyKernel setup start ############### 

# EDIFY properties
do.devicecheck=1
do.initd=0
do.modules=0
do.cleanup=1
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
device.name15=
# end properties

# shell variables
block=/dev/block/platform/msm_sdcc.1/by-name/boot;
add_seandroidenforce=1
supersu_exclusions=""
is_slot_device=0;
ramdisk_compression=auto;

############### AnyKernel setup end ############### 

## AnyKernel methods (DO NOT CHANGE)
# import patching functions/variables - see for reference
. /tmp/anykernel/tools/ak2-core.sh;

## AnyKernel file attributes
# set permissions/ownership for included ramdisk files
chmod -R 750 $ramdisk/*;
chown -R root:root $ramdisk/*;

chmod 775 $ramdisk/res
chmod -R 755 $ramdisk/res/bc
chmod -R 755 $ramdisk/res/misc

# dump current kernel
dump_boot;

############### Ramdisk customization start ###############

mount -o rw,remount /system;

ASD=$(cat /system/build.prop | grep ro.build.version.sdk | cut -d "=" -f 2)

if [ "$ASD" == "24" ] || [ "$ASD" == "25" ]; then
 ui_print "Android 7.0/7.1 detected!";
 touch $ramdisk/nougat;
fi;

remove_line init.qcom.rc scaling_min_freq;
remove_line init.qcom.rc scaling_min_freq;
remove_line init.qcom.rc scaling_min_freq;
remove_line init.qcom.rc scaling_min_freq;
remove_line init.qcom.rc "start mpdecision";
remove_line init.qcom.rc "start mpdecision";

############### Ramdisk customization end ###############

# write new kernel
write_boot;
