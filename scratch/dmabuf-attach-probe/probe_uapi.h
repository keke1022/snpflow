/* SPDX-License-Identifier: GPL-2.0 */
#ifndef CVM_DMABUF_PROBE_UAPI_H
#define CVM_DMABUF_PROBE_UAPI_H

#include <linux/ioctl.h>
#include <linux/types.h>

#define CVM_DMABUF_PROBE_DEV "/dev/cvm_dmabuf_probe"

struct cvm_probe_pci_id {
	__u16 domain;
	__u8 bus;
	__u8 devfn;
};

struct cvm_dmabuf_probe_req {
	__s32 fd;
	__u32 flags;
	struct cvm_probe_pci_id first;
	struct cvm_probe_pci_id second;
	__u32 first_nents;
	__u32 second_nents;
	__u64 first_bytes;
	__u64 second_bytes;
	__u64 first_dma;
	__u64 second_dma;
	__u32 layouts_equal;
	__u32 pad;
};

#define CVM_PROBE_F_SECOND (1U << 0)
#define CVM_DMABUF_PROBE_IOC_MAGIC 'P'
#define CVM_DMABUF_PROBE_IOC_ATTACH \
	_IOWR(CVM_DMABUF_PROBE_IOC_MAGIC, 0x01, struct cvm_dmabuf_probe_req)

#endif
