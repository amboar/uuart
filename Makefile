CFLAGS ?= -O2
CC := arm-linux-gnueabihf-gcc

uuart: uuart.o

.PHONY: clean
clean:
	$(RM) uuart
