# dma-buf multi-device attach probe

Allocates a 2 MiB `cvm_shared` dma-buf and asks the generic Linux dma-buf
framework to attach/map it concurrently to the H200 and ConnectX PCI devices.
It compares the returned SG DMA layouts and immediately unmaps/detaches them.
No GPU or NIC DMA command is submitted.

`probe_user [size_MiB] [iterations]` can exercise larger SG tables and repeated
attachment lifetimes; the allocation is retained across iterations.

Observed on `cvm26-snp`:

```text
2 MiB:   GPU/NIC nents=1,   identical DMA layout
512 MiB: GPU/NIC nents=256, identical DMA layout
512 MiB, 100 attach/map/unmap/detach iterations: all passed, 12.920 us/iteration
```
