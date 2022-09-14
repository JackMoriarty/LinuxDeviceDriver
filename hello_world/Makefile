# @Author: Bangduo Chen <chenbangduo>
# @Date:   2018-02-23T17:10:13+08:00
# @Email:  1413563527@qq.com
# @Filename: Makefile
# @Last modified by:   chenbangduo
# @Last modified time: 2018-02-23T17:20:39+08:00


#判断是否定义了KERNELRELEASE，如果定义了，则说明在代码树代码中，否则在编写的驱动代码中

#在代码树代码中，内核代码书中的makefile将会执行下面语句
ifneq ($(KERNELRELEASE),)
	obj-m := hello_world.o
#在编写的驱动代码中,驱动代码中的makefile将会执行下面语句
else
	KERNELDIR ?=/lib/modules/$(shell uname -r)/build #定义当前代码树的位置
	PWD := $(shell pwd)
default:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) modules
endif

.PHONY: #伪目标
clean:
	rm -rf *.o *.ko .hello_world.* *.symvers *.order *.mod *.mod.c .tmp_versions
