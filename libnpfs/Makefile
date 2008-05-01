SYSNAME:=${shell uname}
SYSNAME!=uname
HFILES=../include/npfs.h npfsimpl.h
CFLAGS=-Wall -g -I ../include

LIBFILES=\
	conn.o\
	error.o\
	fdtrans.o\
	fidpool.o\
	file.o\
	fmt.o\
	mount-$(SYSNAME).o\
	np.o\
	pipesrv.o\
	socksrv.o\
	srv.o\
	trans.o\
	user.o\

libnpfs.a: $(LIBFILES)
	ar rc libnpfs.a $(LIBFILES)
	ranlib libnpfs.a

clean:
	rm -f *.o *.a *~

$(LIBFILES): Makefile $(HFILES)

%.o: %.c 
	$(CC) $(CFLAGS) -c $*.c
