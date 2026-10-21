# Historical DPDK implementation material

The supported installation artifacts are now under
[`../patches/`](../patches/README.md). Use that directory's `series` file.

`INVENTORY.md` records the original survey of DMA allocation and registration
sites. Some implementation descriptions there predate the conservative
fail-stop baseline.

`archive/` contains one-off generators, early helper-source snapshots, and
superseded diffs. They are kept for provenance, not as an alternative build
system. Running a generator can overwrite a current DPDK tree with an old or
rejected implementation.
