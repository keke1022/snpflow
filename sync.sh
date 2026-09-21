#!/usr/bin/env bash
# 把本仓库同步到 CVM 里编译运行。主机是唯一真实来源。
set -euo pipefail
rsync -az --delete --exclude '.git' --exclude 'build' --exclude '*.ko' --exclude '*.o' --exclude '*.mod*' --exclude '.*.cmd' --exclude 'Module.symvers' --exclude 'modules.order' \
    "$(dirname "$(readlink -f "$0")")/" cvm26:~/snp-dpdk/
echo "已同步到 cvm26:~/snp-dpdk/"
