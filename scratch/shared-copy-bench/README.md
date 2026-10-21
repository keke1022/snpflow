# Shared-to-private copy benchmark

Copies a resident 512 MiB `cvm_shared` mapping into an ordinary private
allocation 32 times (16 GiB total), using 1, 4, and 8 pinned worker threads.
The 1 GiB working set is larger than cache and represents the conservative
extra-copy fallback before a native GPU bounce buffer.

Observed on `cvm26-snp` (16 GiB copied per configuration):

```text
1 thread   10.470 GiB/s   89.939 Gb/s
4 threads  34.002 GiB/s  292.075 Gb/s
8 threads  61.123 GiB/s  525.039 Gb/s
```
