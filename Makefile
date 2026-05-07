obj-m += esp32_monitor.o

KDIR := /lib/modules/$(shell uname -r)/build
PWD  := $(shell pwd)

all:
	make -C $(KDIR) M=$(PWD) modules

clean:
	make -C $(KDIR) M=$(PWD) clean

load:
	sudo insmod esp32_monitor.ko

unload:
	sudo rmmod esp32_monitor

reload: unload load

log:
	sudo dmesg | grep ESP32_MONITOR | tail -20

.PHONY: all clean load unload reload log