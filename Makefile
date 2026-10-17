CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra

all: sender receiver

sender: sender.c blast.h garble.h
	$(CC) $(CFLAGS) -pthread -o $@ sender.c

receiver: receiver.c blast.h garble.h
	$(CC) $(CFLAGS) -pthread -o $@ receiver.c

clean:
	rm -f sender receiver

.PHONY: all clean
