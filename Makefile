CFLAGS ?= -O2 -g -Wall -Wextra
LDLIBS = -libverbs

snp-probe: snp-probe.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

clean:
	rm -f snp-probe

.PHONY: clean
