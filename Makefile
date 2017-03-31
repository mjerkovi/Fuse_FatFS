all: cleanall dummy disk
	cc -Wall file_system.c `pkgconf fuse --cflags --libs` -o file_system

load:
	sudo kldload fuse.ko

run:
	sudo ./file_system dummy -d

disk:
	cc -o fs_init fs_init.c
	./fs_init myDisk 12582912
	#dd if=/dev/zero of=myDisk bs=1024*1024*12 count=1

dummy:
	mkdir dummy

unmount:
	sudo umount /dev/fuse

cleanall:
	rm -rf file_system myDisk dummy fs_init

clean:
	rm file_system 
