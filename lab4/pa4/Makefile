CC = clang
CFLAGS = -std=c99 -Wall -Wextra -Werror -pedantic
LDLIBS = -L. -lruntime
SOURCES = main.c ipc.c clock.c mutex.c protocol.c stats.c

pa4: $(SOURCES) node.h ipc.h common.h pa2345.h libruntime.so
	$(CC) $(CFLAGS) $(SOURCES) $(LDLIBS) -o $@

.PHONY: clean
clean:
	rm -f pa4 a.out *.o events.log pipes.log mutex.trace mutex.stats.csv
