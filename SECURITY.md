# Security reporting

SNPFlow is a research prototype. Its validation reports describe specific
tested configurations, not a general security certification or production
support guarantee. Development currently follows `main`; there is no separate
maintained stable-release branch or promised security-update schedule.

## Report a suspected vulnerability

Email the maintainer at **keke83space@gmail.com** with the subject
`[SNPFlow security] <short description>`. Please report privately before
opening a public issue or pull request containing exploit details.

Include the affected commit, component, reproduction steps or a minimal test,
expected and observed behavior, and likely impact. For device-dependent issues,
include the DPDK version, guest kernel, NIC model/firmware, and relevant runtime
options. Remove credentials, private keys, and sensitive workload data from
attachments. Coordinate publication with the maintainer after initial review;
no fixed response or remediation time is guaranteed.

## Scope and assumptions

Reports about shared/private memory ownership, completion validation, DMA
bounds, module lifetime, and authenticated chunk processing are especially
useful. Shared DMA memory and device-written metadata are untrusted. A private
copy alone does not authenticate packet contents; authenticated object transfer
is specific to the `cvmchunk` path.

Known configuration limits and incomplete features are recorded in
[README.md](README.md) and the
[current validation report](docs/VALIDATION-2026-10-20-feature-coverage.md).
Please distinguish a new violation of the documented guarantees from a request
to support a configuration that is currently rejected or unqualified.
