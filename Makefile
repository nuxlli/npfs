CFLAGS=-Wall -g

all:
	make -C libnpfs
	make -C fs

clean:
	make -C libnpfs clean
	make -C fs clean
	rm -f *~ include/*~

install:
	cp include/npfs.h /usr/local/include
	cp libnpfs/libnpfs.a /usr/local/lib

