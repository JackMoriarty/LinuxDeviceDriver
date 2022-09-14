#!/bin/sh
module="scull"
device="scull"

# 移除模块
/sbin/rmmod $module $* || exit 1

# 移除设备节点

rm -f /dev/${device} /dev/${device}[0-3] 
rm -f /dev/${device}priv
rm -f /dev/${device}pipe /dev/${device}pipe[0-3]
rm -f /dev/${device}single
rm -f /dev/${device}uid
rm -f /dev/${device}wuid