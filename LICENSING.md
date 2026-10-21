# Licensing

SNPFlow is a mixed-license source repository. The root [LICENSE](LICENSE)
provides the MIT grant for original project material that is not covered by
an existing file notice or a component scope below. It does not replace
upstream copyrights, relicense DPDK or Linux, or remove their conditions.

## Component scopes

| Material | License | Scope |
|---|---|---|
| Original documentation, figures, build/CI helpers, standalone probes and experiments without another notice | MIT | Default for project-owned material; third-party excerpts retain their original terms |
| `kmod/cvm_shared.c`, `kmod/cvm_shared.h` | GPL-2.0-only | Existing `GPL-2.0` SPDX notices and kernel-module license declaration are retained |
| `scratch/dmabuf-attach-probe/dmabuf_probe.c`, `probe_uapi.h`, `probe_user.c` | GPL-2.0-only | Existing file notices are retained, including the userspace probe |
| `scratch/cvm-chunk-proto/*.c` and `*.h` | BSD-3-Clause | Existing protocol, sender, receiver, and test notices are retained |
| `patches/dpdk-25.11.3/*.patch` | BSD-3-Clause | DPDK changes, new integration files, and upstream context follow the target components' license |
| Code, patch files, and generator scripts in `dpdk-patches/archive/` | BSD-3-Clause | Historical DPDK implementation material, including embedded DPDK code; documentation remains under the default MIT grant |

The `GPL-2.0` identifier in these existing files means version 2 only, not
"version 2 or later". Full component license texts are included in
[LICENSES/GPL-2.0-only.txt](LICENSES/GPL-2.0-only.txt) and
[LICENSES/BSD-3-Clause.txt](LICENSES/BSD-3-Clause.txt).

Original contributions to these BSD components are copyright 2026 SNPFlow
contributors. Existing copyright notices, including those for DPDK authors,
are retained. See [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) for notices
from upstream source files represented by the integration material.

## Headers and built artifacts

`kmod/cvm_shared.h` and `scratch/dmabuf-attach-probe/probe_uapi.h` currently
carry GPL-2.0 notices **without a syscall exception**. This publication does
not add an exception or a new license to those headers. In particular,
`shared-probe.c`, `verify-probe.c`, and `nop-probe.c` include the first header.
The MIT grant for an original source file is not a claim that a binary built
with these headers can be redistributed under MIT alone.

The root license likewise does not cover linked dependencies or generated
DPDK/kernel binaries. Distribute such artifacts with the applicable component
licenses, copyright notices, and corresponding source obligations. This
repository publishes source and patches, not compiled DPDK or kernel modules.

## Review basis

The 2026-10-20 publication review checked repository SPDX/MODULE_LICENSE
notices, local Git provenance, and the DPDK 25.11.3 source headers for files
represented by the patch series and archived generators. The MIT default
applies to project-owned material at the maintainer's direction; the review
does not establish ownership of any undisclosed external contribution.

The DPDK source and its [contribution guidelines](https://doc.dpdk.org/guides/contributing/patches.html#source-license)
identify BSD-3-Clause for core libraries/drivers and GPL-2.0 for kernel
components. Linux's [licensing rules](https://docs.kernel.org/process/license-rules.html)
describe per-file licensing, dual licensing, UAPI exceptions, and the separate
role of `MODULE_LICENSE`. The default MIT grant follows the
[MIT license text](https://opensource.org/license/mit).

For new code, use the license of the component being changed and preserve
all imported notices. Any future UAPI exception or relicensing should be an
explicit, separately reviewed change by the relevant copyright holders.
