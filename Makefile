CC = cc
CFLAGS = -g -Wall -Wpointer-arith -Wreturn-type -Wstrict-prototypes
LIBS = -lccn -lcrypto

PROGRAMS = ccnping ccnpingserver

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

.PHONY: all clean
