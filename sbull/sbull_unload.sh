#!/bin/sh
module="sbull"
device="sbull"

# invoke rmmod with all arguments we got
/sbin/rmmod $module $* || exit 1

# Remove stale nodes
rm -f /dev/${device}[a-d]* /dev/${device}






