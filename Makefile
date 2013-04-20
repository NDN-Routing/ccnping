CC = cc
CFLAGS = -g -Wall -Wpointer-arith -Wreturn-type -Wstrict-prototypes -fPIC -pie
LIBS = -lm -lccn -lcrypto

PROGRAMS = ccnping ccnpingserver

PREFIX=/usr/local
DESTDIR=

all: $(PROGRAMS)

ccnping: ccnping.o
	$(CC) $(CFLAGS) -o $@ ccnping.o $(LIBS)

ccnpingserver: ccnpingserver.o
	$(CC) $(CFLAGS) -o $@ ccnpingserver.o $(LIBS)

clean:
	rm -f *.o
	rm -f $(PROGRAMS)

.c.o:
	$(CC) $(CFLAGS) -c $<

install:
	cp $(PROGRAMS) ${DESTDIR}${PREFIX}/bin

.PHONY: all clean
