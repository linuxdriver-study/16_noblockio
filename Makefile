KERNELDIR := /home/wl/linux_system_study/imx6ull/linux/alientek_linux/linux-imx-rel_imx_4.1.15_2.1.0_ga_alientek
#KERNELDIR := /home/wl/linux_system_study/linux_kernel_alientek
CURRENT_PATH := $(shell pwd)
obj-m := noblockio.o

build : kernel_modules

kernel_modules :
	$(MAKE) -C $(KERNELDIR) M=$(CURRENT_PATH) modules

clean :
	$(MAKE) -C $(KERNELDIR) M=$(CURRENT_PATH) clean
