CC = clang
CFLAGS = -std=c99 -Wall -Wextra -Werror -pedantic
SOURCES = main.c bank.c clock.c history.c ipc.c bank_robbery.c
HEADERS = node.h ipc.h banking.h common.h pa2345.h

.PHONY: all run clean
all: pa3

pa3: $(SOURCES) $(HEADERS) libruntime.so
	$(CC) $(CFLAGS) $(SOURCES) -L. -lruntime -o $@

run: pa3
	LD_LIBRARY_PATH=. LD_PRELOAD=./libruntime.so ./pa3 -p 3 10 20 30

clean:
	rm -f pa3 a.out events.log pipes.log
