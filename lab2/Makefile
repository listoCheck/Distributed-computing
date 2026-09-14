CC ?= clang
CFLAGS ?= -D_DEFAULT_SOURCE -std=c99 -Wall -Wextra -Werror -pedantic

pa2: main.c bank_robbery.c ipc.h banking.h pa2345.h process.h
	$(CC) $(CFLAGS) main.c bank_robbery.c -L. -lruntime -o $@

clean:
	rm -f pa2 events.log pipes.log

package:
	tar -czf pa2.tar.gz --transform 's,^,pa2/,' main.c bank_robbery.c ipc.h banking.h pa2345.h process.h Makefile README.md
