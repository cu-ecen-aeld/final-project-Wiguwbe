
CROSS_COMPILE?=

CC?=$(CROSS_COMPILE)gcc

CFLAGS?=
LDFLAGS?= -lpthread

all: ledserver
default: ledserver

ledserver: main.o
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

main.o: main.c

%.o: %.c
	$(CC) $(CFLAGS) -c $^

clean:
	rm -f *.o ledserver
