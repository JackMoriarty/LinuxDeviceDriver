#!/bin/sh
module="sculld"
device="sculld"

# invoke rmmod with all arguments we got
/sbin/rmmod $module $* || exit 1

# remove nodes
rm -f /dev/${device}[0-3] /dev/${device}

exit 0

