#!/bin/sh
module="sculld"
device="sculld"
mode="664"

# Group: since distributions do it differently, look for wheel or use staff
if grep '^staff:' /etc/group > /dev/null; then
    group="staff"
else
    group="wheel"
fi

# remove stale nodes
rm -f /dev/${device}? 

# invoke insmod with all arguments we got
# and use a pathname, as newer modutils don't look in . by default
/sbin/insmod -f ./$module.ko $* || exit 1

major=$(awk "\$2==\"$module\" {print \$1}" /proc/devices)

mknod /dev/${device}0 c $major 0
mknod /dev/${device}1 c $major 1
mknod /dev/${device}2 c $major 2
mknod /dev/${device}3 c $major 3
ln -sf ${device}0  /dev/${device}

# give appropriate group/permissions
chgrp $group /dev/${device}[0-3]
chmod $mode  /dev/${device}[0-3]

