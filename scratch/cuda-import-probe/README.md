# CUDA import probe

Uses `dlopen()` plus embedded PTX, so no CUDA toolkit or headers are required.
It tests three public driver API paths against a 2 MiB `cvm_shared` allocation:

1. `cuMemHostRegister()` on the userspace mmap, followed by an SM load.
2. `cuImportExternalMemory()` with `DMABUF_FD`, followed by an SM load.
3. A control path that copies 64 bytes from the mapping to HBM with
   `cuMemcpyHtoD()` and then performs the same SM load.

The first two paths distinguish a successful API mapping from actual GPU
visibility. The third path is not zero-copy: in CC mode it exercises CUDA's
protected CPU-to-GPU transfer path.

## H200 result (2026-09-30)

Environment: H200 NVL, driver 595.91.07, SEV-SNP guest, GPU CC mode ON.
The GPU was locally attested with NVIDIA NVAT 1.2.2 before its ReadyState was
set to `ready`.

```text
device attribute CAN_MAP_HOST_MEMORY (19): 1
device attribute PAGEABLE_MEMORY_ACCESS (88): 0
device attribute HOST_REGISTER_SUPPORTED (99): 0
device attribute PAGEABLE_MEMORY_ACCESS_USES_HOST_PAGE_TABLES (100): 0
device attribute READ_ONLY_HOST_REGISTER_SUPPORTED (113): 1
device attribute GPU_DIRECT_RDMA_SUPPORTED (116): 0
device attribute DMA_BUF_SUPPORTED (124): 0
device attribute HOST_ALLOC_DMA_BUF_SUPPORTED (146): 0
cuMemHostRegister(anonymous control): 801 (CUDA_ERROR_NOT_SUPPORTED)
cuMemHostRegister(cvm_shared mmap): 801 (CUDA_ERROR_NOT_SUPPORTED)
cuImportExternalMemory(DMABUF_FD): 801 (CUDA_ERROR_NOT_SUPPORTED)
cuMemcpyHtoD(from shared mmap): 0 (CUDA_SUCCESS)
GPU read after protected CUDA copy: PASS (GPU loaded the expected 64 bytes)
```

The anonymous-memory control rules out a property specific to `cvm_shared` as
the reason for the `cuMemHostRegister()` error.  NVIDIA's Secure AI Operations
Guide documents that host registration returns `CUDA_ERROR_NOT_SUPPORTED` in
both SPT and protected-PCIe modes, and the device reports the corresponding
base capability as disabled.  The more specific read-only attribute being one
does not override the disabled base capability or the CC-mode API restriction.

The external-memory error has a separate cause.  The CUDA Driver API documents
`DMABUF_FD` import as supported only on Tegra starting with Thor, not on an H200
dGPU.  A filtered function trace recorded zero calls to `dma_buf_attach()`,
`nv_dma_import_from_fd()`, or `nv_dma_import_dma_buf()` during the failed CUDA
import.  As a trace control, the generic Linux probe attached the same dma-buf
to H200 and ConnectX successfully and recorded two `dma_buf_attach()` calls.
This places the CUDA rejection before the Linux dma-buf attachment path; the
exporter's SG layout is not what produced this `801`.

Thus neither public API provides direct GPU access to the plaintext shared
mapping on this H200. The supported CUDA copy path can consume the mapping,
but in CC mode the driver stages the transfer through its encrypted and
authenticated bounce-buffer path before the SM reads it from protected HBM.

References:

- [NVIDIA Secure AI Operations Guide](https://docs.nvidia.com/nvidia-secure-ai-operations-guide.pdf)
- [CUDA external-resource interoperability API](https://docs.nvidia.com/cuda/cuda-driver-api/cuda_driver_api/group__CUDA__EXTRES__INTEROP.html)
- [NVIDIA Secure AI with Blackwell and Hopper GPUs](https://docs.nvidia.com/nvidia-secure-ai-with-blackwell-and-hopper-gpus-whitepaper.pdf)
