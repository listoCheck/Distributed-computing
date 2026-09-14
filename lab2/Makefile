CC = clang
CFLAGS = -std=c99 -Wall -Wextra -Werror -pedantic

pa2: main.c ipc.c bank_robbery.c ipc.h banking.h pa2345.h common.h process.h
	$(CC) $(CFLAGS) main.c ipc.c bank_robbery.c -L. -lruntime -o $@

clean:
	rm -f pa2 events.log pipes.log

test: pa2
	python3 tests/test.py

package:
	tar -czf pa2.tar.gz --transform 's,^,pa2/,' main.c ipc.c bank_robbery.c ipc.h banking.h pa2345.h common.h process.h Makefile README.md
