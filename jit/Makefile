# /*
#  * @Author: Bangduo Chen 
#  * @Date: 2018-09-18 15:17:02 
#  * @Last Modified by:   Bangduo Chen 
#  * @Last Modified time: 2018-09-18 15:17:02 
#  */

ifneq ($(KERNELRELEASE),)
obj-m = jit.o jiq.o
else
KERNELDIR ?=/lib/modules/$(shell uname -r)/build
PWD	:= $(shell pwd)

modules:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) modules
endif

clean:
	rm -rf *.o *~ core .depend .*.cmd *.ko *.mod.c .tmp_versions

depend .depend dep:
	$(CC) $(CFLAGS) -M *.c > .depend

ifeq (.depend, $(wildcard .depend))
	include .depend
endif