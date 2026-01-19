obj-m += vtfs.o 
vtfs-objs := fs/fs.o fs/inode.o fs/dir.o fs/file.o fs/store.o

PWD := $(CURDIR) 
KDIR = /lib/modules/`uname -r`/build
EXTRA_CFLAGS = -Wall -g

all:
	make -C $(KDIR) M=$(PWD) modules 

clean:
	make -C $(KDIR) M=$(PWD) clean
	rm -rf .cache
