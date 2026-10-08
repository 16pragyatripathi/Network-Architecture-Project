CFLAGS += -std=c11 -O2 -Wall -Wextra -Wpedantic -D_DEFAULT_SOURCE

all: bserve bcurl

bserve: src/bserve.c
	$(CC) $(CFLAGS) -o $@ src/bserve.c

bcurl: src/bcurl.c
	$(CC) $(CFLAGS) -o $@ src/bcurl.c

test: all
	python3 -m unittest discover -s tests -v

clean:
	rm -f bserve bcurl

.PHONY: all test clean
