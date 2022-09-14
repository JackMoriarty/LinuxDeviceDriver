#!/bin/sh
module="scull"
device="scull"
mode="664"      #rw-rw-r--

# 配置设备文件的用户组
# 由于不同的发行版不同
if grep -q '^staff:' /etc/group; then
    group="staff"
else
    group="wheel"
fi

# 安装模块,将脚本获得的参数传给安装命令,失败则退出
/sbin/insmod ./$module.ko $* || exit 1

# 由于是动态获取主设备号,所以在此处获取主设备号
major=$(awk "\$2==\"$module\" {print \$1}" /proc/devices)

# 创建对应的设备文件
# scull
rm -rf /dev/${device}[0-3]
mknod /dev/${device}0 c $major 0
mknod /dev/${device}1 c $major 1
mknod /dev/${device}2 c $major 2
mknod /dev/${device}3 c $major 3
ln -sf /dev/${device}0 /dev/${device}
chgrp $group /dev/${device}[0-3]
chmod $mode /dev/${device}[0-3]

# scullpipe
rm -rf /dev/${device}pipe[0-3]
mknod /dev/${device}pipe0 c $major 4
mknod /dev/${device}pipe1 c $major 5
mknod /dev/${device}pipe2 c $major 6
mknod /dev/${device}pipe3 c $major 7
ln -sf /dev/${device}pipe0 /dev/${device}pipe
chgrp $group /dev/${device}pipe[0-3]
chmod $mode /dev/${device}pipe[0-3]

# scullsingle
rm -f /dev/${device}single
mknod /dev/${device}single  c $major 8
chgrp $group /dev/${device}single
chmod $mode  /dev/${device}single

# sculluid
rm -f /dev/${device}uid
mknod /dev/${device}uid   c $major 9
chgrp $group /dev/${device}uid
chmod $mode  /dev/${device}uid

# scullwuid
rm -f /dev/${device}wuid
mknod /dev/${device}wuid  c $major 10
chgrp $group /dev/${device}wuid
chmod $mode  /dev/${device}wuid

#scullpriv
rm -f /dev/${device}priv
mknod /dev/${device}priv  c $major 11
chgrp $group /dev/${device}priv
chmod $mode  /dev/${device}priv