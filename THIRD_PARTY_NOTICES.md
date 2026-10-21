# Third-party notices

The DPDK integration patches and archived implementation material contain
changes to and excerpts from DPDK 25.11.3. The notices below are retained from
the corresponding upstream source headers in imported baseline tree
`d09d6d6f46cef3d6234ba7f11ee7e187e0ca4e02`. They identify the upstream material;
they do not attribute SNPFlow's new code to those authors.

The full [BSD-3-Clause conditions and disclaimer](LICENSES/BSD-3-Clause.txt)
apply to this material. These notices supplement the original headers left
in the DPDK tree when applying the patches. Original SNPFlow contributions
are copyright 2026 SNPFlow contributors; component scopes are specified in
[LICENSING.md](LICENSING.md).

The GPL-2.0 and BSD-3-Clause license texts in `LICENSES/` were copied verbatim
from the corresponding `license/` files of that DPDK source tree. The GPL
license text retains the Free Software Foundation's own copyright notice.

DPDK, rdma-core, OpenSSL, Linux kernel headers, Python cryptography, and the
NVIDIA driver are external build/runtime dependencies. Their complete source
or binaries are not vendored here. Their own licenses apply when distributing
them or artifacts incorporating them.

## Upstream DPDK source notices

### `app/test-pmd/meson.build`

```text
# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2017 Intel Corporation
# override default name to drop the hyphen
```

### `app/test-pmd/testpmd.c`

```text
/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2010-2017 Intel Corporation
 */
```

### `app/test-pmd/testpmd.h`

```text
/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2010-2017 Intel Corporation
 */
```

### `app/test/meson.build`

```text
# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2017-2023 Intel Corporation
# the main test files [test.c and commands.c] relies on these libraries
# optional dependencies: some files may use these - and so we should link them in -
# but do not explicitly require them so they are not listed in the per-file lists below
# some other utility C files, providing functions used by various tests
```

### `drivers/common/mlx5/linux/meson.build`

```text
# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2020 Mellanox Technologies, Ltd
```

### `drivers/common/mlx5/linux/mlx5_common_os.c`

```text
/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright 2020 Mellanox Technologies, Ltd
 */
```

### `drivers/common/mlx5/linux/mlx5_common_os.h`

```text
/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright 2020 Mellanox Technologies, Ltd
 */
```

### `drivers/common/mlx5/linux/mlx5_common_verbs.c`

```text
/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright 2020 Mellanox Technologies, Ltd
 */
```

### `drivers/common/mlx5/linux/mlx5_glue.c`

```text
/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright 2018 6WIND S.A.
 * Copyright 2018 Mellanox Technologies, Ltd
 */
```

### `drivers/common/mlx5/linux/mlx5_glue.h`

```text
/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright 2018 6WIND S.A.
 * Copyright 2018 Mellanox Technologies, Ltd
 */
```

### `drivers/common/mlx5/mlx5_common.c`

```text
/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright 2019 Mellanox Technologies, Ltd
 */
```

### `drivers/common/mlx5/mlx5_common_devx.c`

```text
/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright 2020 Mellanox Technologies, Ltd
 */
```

### `drivers/common/mlx5/mlx5_common_mr.c`

```text
/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright 2016 6WIND S.A.
 * Copyright 2020 Mellanox Technologies, Ltd
 */
```

### `drivers/common/mlx5/mlx5_malloc.c`

```text
/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright 2020 Mellanox Technologies, Ltd
 */
```

### `drivers/common/mlx5/mlx5_malloc.h`

```text
/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright 2020 Mellanox Technologies, Ltd
 */
```

### `drivers/net/mlx5/hws/mlx5dr_send.c`

```text
/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2022 NVIDIA Corporation & Affiliates
 */
```

### `drivers/net/mlx5/hws/mlx5dr_send.h`

```text
/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2022 NVIDIA Corporation & Affiliates
 */
```

### `drivers/net/mlx5/linux/mlx5_os.c`

```text
/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright 2015 6WIND S.A.
 * Copyright 2020 Mellanox Technologies, Ltd
 */
```

### `drivers/net/mlx5/linux/mlx5_verbs.c`

```text
/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright 2020 Mellanox Technologies, Ltd
 */
```

### `drivers/net/mlx5/mlx5_devx.c`

```text
/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright 2020 Mellanox Technologies, Ltd
 */
```

### `drivers/net/mlx5/mlx5_flow_aso.c`

```text
/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright 2020 Mellanox Technologies, Ltd
 */
```

### `drivers/net/mlx5/mlx5_flow_quota.c`

```text
/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Nvidia Inc. All rights reserved.
 */
```

### `drivers/net/mlx5/mlx5_hws_cnt.c`

```text
/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright 2020 Mellanox Technologies, Ltd
 */
```

### `drivers/net/mlx5/mlx5_rx.c`

```text
/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright 2021 6WIND S.A.
 * Copyright 2021 Mellanox Technologies, Ltd
 */
```

### `drivers/net/mlx5/mlx5_rx.h`

```text
/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright 2021 6WIND S.A.
 * Copyright 2021 Mellanox Technologies, Ltd
 */
```

### `drivers/net/mlx5/mlx5_rxq.c`

```text
/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright 2015 6WIND S.A.
 * Copyright 2015 Mellanox Technologies, Ltd
 */
```

### `drivers/net/mlx5/mlx5_rxtx_vec.c`

```text
/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright 2017 6WIND S.A.
 * Copyright 2017 Mellanox Technologies, Ltd
 */
```

### `drivers/net/mlx5/mlx5_rxtx_vec_altivec.h`

```text
/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright 2017 6WIND S.A.
 * Copyright 2017 Mellanox Technologies, Ltd
 */
```

### `drivers/net/mlx5/mlx5_rxtx_vec_neon.h`

```text
/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright 2017 6WIND S.A.
 * Copyright 2017 Mellanox Technologies, Ltd
 */
```

### `drivers/net/mlx5/mlx5_rxtx_vec_sse.h`

```text
/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright 2017 6WIND S.A.
 * Copyright 2017 Mellanox Technologies, Ltd
 */
```

### `drivers/net/mlx5/mlx5_trigger.c`

```text
/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright 2015 6WIND S.A.
 * Copyright 2015 Mellanox Technologies, Ltd
 */
```

### `drivers/net/mlx5/mlx5_tx.c`

```text
/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright 2021 6WIND S.A.
 * Copyright 2021 Mellanox Technologies, Ltd
 */
```

### `drivers/net/mlx5/mlx5_tx.h`

```text
/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright 2021 6WIND S.A.
 * Copyright 2021 Mellanox Technologies, Ltd
 */
```

### `drivers/net/mlx5/mlx5_txq.c`

```text
/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright 2015 6WIND S.A.
 * Copyright 2015 Mellanox Technologies, Ltd
 */
```
