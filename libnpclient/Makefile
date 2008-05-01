SYSNAME:=${shell uname}
SYSNAME!=uname
HFILES=../include/npfs.h ../include/npclient.h npcimpl.h
CFLAGS=-Wall -g -I ../include

LIBFILES=\
	dirread.o\
	fid.o\
	fsys.o\
	mount.o\
	netmount.o\
	open.o\
	pool.o\
	read.o\
	remove.o\
	stat.o\
	stat2wstat.o\
	walk.o\
	write.o\

libnpclient.a: $(LIBFILES)
	ar rc libnpclient.a $(LIBFILES)
	ranlib libnpclient.a

clean:
	rm -f *.o *.a *~
	make -C examples clean

$(LIBFILES): Makefile $(HFILES)

%.o: %.c 
	$(CC) $(CFLAGS) -c $*.c
