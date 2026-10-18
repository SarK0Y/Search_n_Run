CC ?= cc
CFLAGS ?= -std=c11 -Wall -Wextra -O2

.PHONY: all run clean

all: srchrun

srchrun: srchrun.c
	$(CC) $(CFLAGS) -o $@ $<

run: srchrun
	./srchrun

clean:
	rm -f srchrun
 
