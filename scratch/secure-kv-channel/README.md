# Hostile-DMA-safe KV channel prototype

This prototype explores a userspace-only alternative to modifying NVIDIA UVM.
It treats every `cvm_shared` page, CQE and device-written field as an untrusted
transport rather than trusted process memory.

## Mechanism

1. The sender emits constant-size, independently authenticated KV cells.
2. DPDK posts a fixed SPRQ ring. Private queue state determines the next slot;
   the public CQE is only a wake-up hint.
3. The receiver copies exactly one constant-size cell to private memory or
   protected HBM before interpreting variable metadata.
4. Stream, sequence, key epoch, true length and tensor destination are AEAD AAD
   and are also checked against private receiver state.
5. The final KV tensor is updated only after authentication succeeds.

The intended GPU implementation does not map `cvm_shared` into H200. It uses
the supported CUDA path:

```text
ConnectX -> public ciphertext cell
         -> fixed-size snapshot / cudaMemcpyAsync
         -> protected HBM ciphertext
         -> GPU authenticate
         -> commit plaintext to final HBM KV layout
```

If CUDA cannot copy directly from the `remap_pfn_range` mapping, the conservative
fallback is an exact-size CPU copy into ordinary CUDA pinned private memory,
then the same native CUDA H2D path. No NVIDIA kernel module change is required.

## Executable invariants

Run:

```bash
python3 secure_kv_channel.py
```

The model injects forged completion lengths/generations, forged slot indices,
ciphertext corruption, a mutation after snapshot and a valid old replay. It
checks that none can redirect a memory access, advance private state, or modify
the final KV destination before authentication.

## Deliberate scope limits

- This is a protocol/state-machine model, not a CUDA or DPDK benchmark.
- The ordered-slot rule assumes scalar SPRQ with CQE compression, MPRQ and
  scatter disabled, matching the current conservative CVM configuration.
- A malicious host can always cause denial of service. This mechanism aims to
  reduce corruption, disclosure and memory-safety attacks to detectable drops.
- A production GPU kernel must either authenticate in a first pass and decrypt
  directly in a second pass, or decrypt into scratch HBM and commit afterward.
  It must never expose unauthenticated plaintext in the live KV tensor.
