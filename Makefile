BIN = ~/mips-gcc472-glibc216-64bit-master/bin
PRE = mips-linux-gnu
CC  = $(BIN)/$(PRE)-gcc
LD  = $(BIN)/$(PRE)-ld
CFLAGS = -std=c99 -pthread -fPIC -nostartfiles -Isrc
LDFLAGS = -shared -ldl
DIST = dist

build:
	mkdir -p $(DIST)
	$(CC) $(CFLAGS) -c src/main.c -o $(DIST)/main.o
	$(LD) $(LDFLAGS) -o $(DIST)/libimp.so $(DIST)/main.o

clean:
	rm -rf $(DIST)

.PHONY: build clean
