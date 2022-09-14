# 取消下面的注释将启用调试
DEBUG = y

# 向CFLAGS添加调试参数
ifeq ($(DEBUG),y)
	DEBFLAGS = -O -g -DSCULL_DEBUG 	# -O用来进行内联展开
else
	DEBFLAGS = -O2
endif

CFLAGS += $(DEBFLAGS)
#CFLAGS += -I$(LDDINC)

ifneq ($(KERNELRELEASE),)
# 在内核源代码树构建系统调用

scull-objs := main.o pipe.o access.o
obj-m := scull.o

else 

KERNELDIR ?= /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

modules:
	#$(MAKE) -C $(KERNELDIR) M=$(PWD) LDDINC=$(PWD)/../include modules
	$(MAKE) -C $(KERNELDIR) M=$(PWD) modules
endif

clean:
	rm -rf *.o *~ core .depend .*.cmd *.ko *.mod.c .tmp_versions

depend .depend dep:
	$(CC) $(CFLAGS) -M *.c > .depend

ifeq (.depend, $(wildcard .depend))
	include .depend
endif
