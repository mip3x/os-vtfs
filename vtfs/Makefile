obj-m += vtfs.o 
vtfs-objs := source/fs.o source/inode.o source/dir.o source/file.o source/store.o

PWD := $(CURDIR) 
KDIR = /lib/modules/`uname -r`/build
EXTRA_CFLAGS = -Wall -g

all:
	make -C $(KDIR) M=$(PWD) modules 

clean:
	make -C $(KDIR) M=$(PWD) clean
	rm -rf .cache
