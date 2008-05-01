CFLAGS=-Wall -g

all:
	make -C libnpfs
	make -C fs

clean:
	make -C libnpfs clean
	make -C fs clean
	rm -f *~ include/*~

