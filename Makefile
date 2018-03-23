MODULE_NAME = elevator
PWD := $(shell pwd)

KDIR := /lib/modules/`uname -r`/build


obj-y := sys_start_elevator.o
obj-y += sys_stop_elevator.o
obj-y += sys_issue_request.o


$(MODULE_NAME)-objs += elevator_proc.o
obj-m :=$(MODULE_NAME).o


default:
	$(MAKE) -C $(KDIR) SUBDIRS=$(PWD) M=$(PWD) modules
clean:
	rm -f *.ko *.o Module* *mod*
