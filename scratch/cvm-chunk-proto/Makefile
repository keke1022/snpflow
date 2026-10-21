CC ?= cc
CPPFLAGS += $(shell pkg-config --cflags openssl)
CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -Wall -Wextra -Werror -Wformat=2 -Wshadow
LDLIBS += $(shell pkg-config --libs openssl)

COMMON_SOURCES = cvm_chunk_wire.c cvm_chunk_crypto.c cvm_chunk_packet.c \
		 cvm_chunk_receiver.c
COMMON_HEADERS = cvm_chunk_wire.h cvm_chunk_crypto.h cvm_chunk_packet.h \
		 cvm_chunk_receiver.h

.PHONY: all clean test

all: cvm_chunk_test cvm_chunk_send

cvm_chunk_test: cvm_chunk_test.c $(COMMON_SOURCES) $(COMMON_HEADERS)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ cvm_chunk_test.c $(COMMON_SOURCES) $(LDLIBS)

cvm_chunk_send: cvm_chunk_send.c cvm_chunk_wire.c cvm_chunk_crypto.c \
		cvm_chunk_packet.c $(COMMON_HEADERS)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ cvm_chunk_send.c cvm_chunk_wire.c \
		cvm_chunk_crypto.c cvm_chunk_packet.c $(LDLIBS)

test: cvm_chunk_test
	./cvm_chunk_test

clean:
	rm -f cvm_chunk_test cvm_chunk_send
