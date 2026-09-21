CFLAGS ?= -O2 -g -Wall -Wextra
BINS = snp-probe devx-probe shared-probe

all: $(BINS)

snp-probe: snp-probe.c
	$(CC) $(CFLAGS) -o $@ $< -libverbs

shared-probe: shared-probe.c
	$(CC) $(CFLAGS) -o $@ $< -libverbs -lmlx5

devx-probe: devx-probe.c
	$(CC) $(CFLAGS) -o $@ $< -libverbs -lmlx5

clean:
	rm -f $(BINS)

.PHONY: all clean
