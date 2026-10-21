// SPDX-License-Identifier: MIT
/* Probe public CUDA paths for an externally allocated cvm_shared dma-buf. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/ioctl.h>
#include <linux/types.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#define CVM_SHARED_DEV "/dev/cvm_shared"
struct cvm_shared_alloc {
	__u64 size;
	__u32 flags;
	__s32 fd;
};
#define CVM_SHARED_IOC_MAGIC 'C'
#define CVM_SHARED_IOC_ALLOC \
	_IOWR(CVM_SHARED_IOC_MAGIC, 0x01, struct cvm_shared_alloc)

typedef int CUresult;
typedef int CUdevice;
typedef void *CUcontext;
typedef void *CUexternalMemory;
typedef void *CUmodule;
typedef void *CUfunction;
typedef unsigned long long CUdeviceptr;

#define CUDA_SUCCESS 0
#define CU_DEVICE_ATTRIBUTE_CAN_MAP_HOST_MEMORY 19
#define CU_DEVICE_ATTRIBUTE_PAGEABLE_MEMORY_ACCESS 88
#define CU_DEVICE_ATTRIBUTE_HOST_REGISTER_SUPPORTED 99
#define CU_DEVICE_ATTRIBUTE_PAGEABLE_MEMORY_ACCESS_USES_HOST_PAGE_TABLES 100
#define CU_DEVICE_ATTRIBUTE_READ_ONLY_HOST_REGISTER_SUPPORTED 113
#define CU_DEVICE_ATTRIBUTE_GPU_DIRECT_RDMA_SUPPORTED 116
#define CU_DEVICE_ATTRIBUTE_DMA_BUF_SUPPORTED 124
#define CU_DEVICE_ATTRIBUTE_HOST_ALLOC_DMA_BUF_SUPPORTED 146
#define CU_MEMHOSTREGISTER_PORTABLE 0x01
#define CU_MEMHOSTREGISTER_DEVICEMAP 0x02
#define CU_EXTERNAL_MEMORY_HANDLE_TYPE_DMABUF_FD 9

typedef struct {
	int type;
	union {
		int fd;
		struct {
			void *handle;
			const void *name;
		} win32;
		const void *nvSciBufObject;
	} handle;
	unsigned long long size;
	unsigned int flags;
	unsigned int reserved[16];
} CUDA_EXTERNAL_MEMORY_HANDLE_DESC;

typedef struct {
	unsigned long long offset;
	unsigned long long size;
	unsigned int flags;
	unsigned int reserved[16];
} CUDA_EXTERNAL_MEMORY_BUFFER_DESC;

struct cuda_api {
	CUresult (*init)(unsigned int);
	CUresult (*device_get)(CUdevice *, int);
	CUresult (*device_get_attribute)(int *, int, CUdevice);
	CUresult (*primary_retain)(CUcontext *, CUdevice);
	CUresult (*primary_release)(CUdevice);
	CUresult (*ctx_set_current)(CUcontext);
	CUresult (*host_register)(void *, size_t, unsigned int);
	CUresult (*host_get_ptr)(CUdeviceptr *, void *, unsigned int);
	CUresult (*host_unregister)(void *);
	CUresult (*import_external)(CUexternalMemory *,
				    const CUDA_EXTERNAL_MEMORY_HANDLE_DESC *);
	CUresult (*external_get_buffer)(CUdeviceptr *, CUexternalMemory,
					const CUDA_EXTERNAL_MEMORY_BUFFER_DESC *);
	CUresult (*destroy_external)(CUexternalMemory);
	CUresult (*mem_alloc)(CUdeviceptr *, size_t);
	CUresult (*mem_free)(CUdeviceptr);
	CUresult (*memcpy_htod)(CUdeviceptr, const void *, size_t);
	CUresult (*memcpy_dtoh)(void *, CUdeviceptr, size_t);
	CUresult (*module_load_data)(CUmodule *, const void *);
	CUresult (*module_get_function)(CUfunction *, CUmodule, const char *);
	CUresult (*module_unload)(CUmodule);
	CUresult (*launch_kernel)(CUfunction, unsigned int, unsigned int,
				  unsigned int, unsigned int, unsigned int,
				  unsigned int, unsigned int, void *, void **,
				  void **);
	CUresult (*ctx_synchronize)(void);
	CUresult (*error_name)(CUresult, const char **);
	CUresult (*error_string)(CUresult, const char **);
};

static int load_symbol(void *lib, void **out, const char *name)
{
	*out = dlsym(lib, name);
	if (!*out) {
		fprintf(stderr, "missing CUDA symbol %s: %s\n", name, dlerror());
		return -1;
	}
	return 0;
}

#define LOAD(api, member, lib, symbol) \
	load_symbol((lib), (void **)&(api).member, (symbol))

static void print_result(struct cuda_api *api, const char *operation,
			 CUresult result)
{
	const char *name = "unknown";
	const char *text = "unknown";

	if (api->error_name)
		api->error_name(result, &name);
	if (api->error_string)
		api->error_string(result, &text);
	printf("%s: %d (%s: %s)\n", operation, result,
	       name ? name : "unknown", text ? text : "unknown");
}

static void print_device_attribute(struct cuda_api *api, CUdevice device,
				   int attribute, const char *name)
{
	int value = -1;
	CUresult result = api->device_get_attribute(&value, attribute, device);

	if (result == CUDA_SUCCESS)
		printf("device attribute %s (%d): %d\n", name, attribute, value);
	else
		print_result(api, name, result);
}

/*
 * Keep the kernel deliberately small: copy the first cache line through an
 * SM into ordinary device memory.  A matching result proves that a GPU load,
 * rather than only a CUDA import/mapping call, reached the source address.
 */
static const char copy_probe_ptx[] =
	".version 8.0\n"
	".target sm_90\n"
	".address_size 64\n"
	".visible .entry copy_probe(\n"
	"    .param .u64 src,\n"
	"    .param .u64 dst)\n"
	"{\n"
	"    .reg .b64 %srcp, %dstp, %v;\n"
	"    ld.param.u64 %srcp, [src];\n"
	"    ld.param.u64 %dstp, [dst];\n"
	"    ld.global.u64 %v, [%srcp+0];\n"
	"    st.global.u64 [%dstp+0], %v;\n"
	"    ld.global.u64 %v, [%srcp+8];\n"
	"    st.global.u64 [%dstp+8], %v;\n"
	"    ld.global.u64 %v, [%srcp+16];\n"
	"    st.global.u64 [%dstp+16], %v;\n"
	"    ld.global.u64 %v, [%srcp+24];\n"
	"    st.global.u64 [%dstp+24], %v;\n"
	"    ld.global.u64 %v, [%srcp+32];\n"
	"    st.global.u64 [%dstp+32], %v;\n"
	"    ld.global.u64 %v, [%srcp+40];\n"
	"    st.global.u64 [%dstp+40], %v;\n"
	"    ld.global.u64 %v, [%srcp+48];\n"
	"    st.global.u64 [%dstp+48], %v;\n"
	"    ld.global.u64 %v, [%srcp+56];\n"
	"    st.global.u64 [%dstp+56], %v;\n"
	"    ret;\n"
	"}\n";

static int gpu_read_and_verify(struct cuda_api *api, CUfunction function,
			       const char *path, CUdeviceptr source,
			       const unsigned char expected[64])
{
	unsigned char observed[64] = {0};
	CUdeviceptr output = 0;
	void *args[] = { &source, &output };
	CUresult result;
	int ret = -1;

	result = api->mem_alloc(&output, sizeof(observed));
	print_result(api, "cuMemAlloc(readback)", result);
	if (result != CUDA_SUCCESS)
		return -1;

	result = api->launch_kernel(function, 1, 1, 1, 1, 1, 1, 0, NULL,
				    args, NULL);
	print_result(api, path, result);
	if (result != CUDA_SUCCESS)
		goto out;

	result = api->ctx_synchronize();
	print_result(api, "cuCtxSynchronize", result);
	if (result != CUDA_SUCCESS)
		goto out;

	result = api->memcpy_dtoh(observed, output, sizeof(observed));
	print_result(api, "cuMemcpyDtoH(readback)", result);
	if (result != CUDA_SUCCESS)
		goto out;

	if (memcmp(observed, expected, sizeof(observed)) != 0) {
		size_t i;

		for (i = 0; i < sizeof(observed); i++) {
			if (observed[i] != expected[i]) {
				printf("%s: MISMATCH at byte %zu: got=%02x want=%02x\n",
				       path, i, observed[i], expected[i]);
				break;
			}
		}
	} else {
		printf("%s: PASS (GPU loaded the expected 64 bytes)\n", path);
		ret = 0;
	}

out:
	api->mem_free(output);
	return ret;
}

int main(void)
{
	const size_t size = 2U << 20;
	struct cvm_shared_alloc alloc = { .size = size };
	CUDA_EXTERNAL_MEMORY_HANDLE_DESC ext_desc = {0};
	CUDA_EXTERNAL_MEMORY_BUFFER_DESC buf_desc = {0};
	struct cuda_api api = {0};
	CUexternalMemory external = NULL;
	CUdeviceptr device_ptr = 0;
	CUdeviceptr copy_ptr = 0;
	CUcontext context = NULL;
	CUmodule module = NULL;
	CUfunction function = NULL;
	CUdevice device;
	CUresult result;
	unsigned char expected[64];
	unsigned char *bytes;
	void *anonymous = MAP_FAILED;
	void *mapping;
	void *lib;
	size_t i;
	int cvm_fd, import_fd = -1;

	cvm_fd = open(CVM_SHARED_DEV, O_RDWR | O_CLOEXEC);
	if (cvm_fd < 0 || ioctl(cvm_fd, CVM_SHARED_IOC_ALLOC, &alloc) != 0) {
		perror("allocate cvm_shared");
		return 1;
	}
	mapping = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED,
		       alloc.fd, 0);
	if (mapping == MAP_FAILED) {
		perror("mmap cvm_shared");
		return 1;
	}
	bytes = mapping;
	for (i = 0; i < size; i++)
		bytes[i] = (unsigned char)((i * 131U + 17U) & 0xffU);
	memcpy(expected, mapping, sizeof(expected));

	lib = dlopen("libcuda.so.1", RTLD_NOW | RTLD_LOCAL);
	if (!lib) {
		fprintf(stderr, "dlopen libcuda: %s\n", dlerror());
		return 1;
	}
	if (LOAD(api, init, lib, "cuInit") ||
	    LOAD(api, device_get, lib, "cuDeviceGet") ||
	    LOAD(api, device_get_attribute, lib, "cuDeviceGetAttribute") ||
	    LOAD(api, primary_retain, lib, "cuDevicePrimaryCtxRetain") ||
	    LOAD(api, primary_release, lib, "cuDevicePrimaryCtxRelease_v2") ||
	    LOAD(api, ctx_set_current, lib, "cuCtxSetCurrent") ||
	    LOAD(api, host_register, lib, "cuMemHostRegister_v2") ||
	    LOAD(api, host_get_ptr, lib, "cuMemHostGetDevicePointer_v2") ||
	    LOAD(api, host_unregister, lib, "cuMemHostUnregister") ||
	    LOAD(api, import_external, lib, "cuImportExternalMemory") ||
	    LOAD(api, external_get_buffer, lib, "cuExternalMemoryGetMappedBuffer") ||
	    LOAD(api, destroy_external, lib, "cuDestroyExternalMemory") ||
	    LOAD(api, mem_alloc, lib, "cuMemAlloc_v2") ||
	    LOAD(api, mem_free, lib, "cuMemFree_v2") ||
	    LOAD(api, memcpy_htod, lib, "cuMemcpyHtoD_v2") ||
	    LOAD(api, memcpy_dtoh, lib, "cuMemcpyDtoH_v2") ||
	    LOAD(api, module_load_data, lib, "cuModuleLoadData") ||
	    LOAD(api, module_get_function, lib, "cuModuleGetFunction") ||
	    LOAD(api, module_unload, lib, "cuModuleUnload") ||
	    LOAD(api, launch_kernel, lib, "cuLaunchKernel") ||
	    LOAD(api, ctx_synchronize, lib, "cuCtxSynchronize") ||
	    LOAD(api, error_name, lib, "cuGetErrorName") ||
	    LOAD(api, error_string, lib, "cuGetErrorString"))
		return 1;

	result = api.init(0);
	print_result(&api, "cuInit", result);
	if (result != CUDA_SUCCESS)
		goto out;
	result = api.device_get(&device, 0);
	print_result(&api, "cuDeviceGet", result);
	if (result != CUDA_SUCCESS)
		goto out;
	print_device_attribute(&api, device,
			       CU_DEVICE_ATTRIBUTE_CAN_MAP_HOST_MEMORY,
			       "CAN_MAP_HOST_MEMORY");
	print_device_attribute(&api, device,
			       CU_DEVICE_ATTRIBUTE_PAGEABLE_MEMORY_ACCESS,
			       "PAGEABLE_MEMORY_ACCESS");
	print_device_attribute(&api, device,
			       CU_DEVICE_ATTRIBUTE_HOST_REGISTER_SUPPORTED,
			       "HOST_REGISTER_SUPPORTED");
	print_device_attribute(&api, device,
			       CU_DEVICE_ATTRIBUTE_PAGEABLE_MEMORY_ACCESS_USES_HOST_PAGE_TABLES,
			       "PAGEABLE_MEMORY_ACCESS_USES_HOST_PAGE_TABLES");
	print_device_attribute(&api, device,
			       CU_DEVICE_ATTRIBUTE_READ_ONLY_HOST_REGISTER_SUPPORTED,
			       "READ_ONLY_HOST_REGISTER_SUPPORTED");
	print_device_attribute(&api, device,
			       CU_DEVICE_ATTRIBUTE_GPU_DIRECT_RDMA_SUPPORTED,
			       "GPU_DIRECT_RDMA_SUPPORTED");
	print_device_attribute(&api, device,
			       CU_DEVICE_ATTRIBUTE_DMA_BUF_SUPPORTED,
			       "DMA_BUF_SUPPORTED");
	print_device_attribute(&api, device,
			       CU_DEVICE_ATTRIBUTE_HOST_ALLOC_DMA_BUF_SUPPORTED,
			       "HOST_ALLOC_DMA_BUF_SUPPORTED");
	result = api.primary_retain(&context, device);
	print_result(&api, "cuDevicePrimaryCtxRetain", result);
	if (result != CUDA_SUCCESS)
		goto out;
	result = api.ctx_set_current(context);
	print_result(&api, "cuCtxSetCurrent", result);
	if (result != CUDA_SUCCESS)
		goto out;
	result = api.module_load_data(&module, copy_probe_ptx);
	print_result(&api, "cuModuleLoadData(copy_probe)", result);
	if (result != CUDA_SUCCESS)
		goto out;
	result = api.module_get_function(&function, module, "copy_probe");
	print_result(&api, "cuModuleGetFunction(copy_probe)", result);
	if (result != CUDA_SUCCESS)
		goto out;

	anonymous = mmap(NULL, size, PROT_READ | PROT_WRITE,
			 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (anonymous == MAP_FAILED) {
		perror("mmap anonymous control");
	} else {
		memset(anonymous, 0xa5, size);
		result = api.host_register(anonymous, size, 0);
		print_result(&api, "cuMemHostRegister(anonymous control)", result);
		if (result == CUDA_SUCCESS)
			api.host_unregister(anonymous);
	}

	result = api.host_register(mapping, size,
				   CU_MEMHOSTREGISTER_PORTABLE |
				   CU_MEMHOSTREGISTER_DEVICEMAP);
	print_result(&api, "cuMemHostRegister(cvm_shared mmap)", result);
	if (result == CUDA_SUCCESS) {
		result = api.host_get_ptr(&device_ptr, mapping, 0);
		print_result(&api, "cuMemHostGetDevicePointer", result);
		if (result == CUDA_SUCCESS)
			gpu_read_and_verify(&api, function,
					    "GPU read via registered shared mmap",
					    device_ptr, expected);
		api.host_unregister(mapping);
	}

	import_fd = dup(alloc.fd);
	if (import_fd < 0) {
		perror("dup dma-buf fd");
		goto out;
	}
	ext_desc.type = CU_EXTERNAL_MEMORY_HANDLE_TYPE_DMABUF_FD;
	ext_desc.handle.fd = import_fd;
	ext_desc.size = size;
	result = api.import_external(&external, &ext_desc);
	print_result(&api, "cuImportExternalMemory(DMABUF_FD)", result);
	if (result == CUDA_SUCCESS) {
		/* CUDA owns the imported descriptor after a successful import. */
		import_fd = -1;
		buf_desc.size = size;
		result = api.external_get_buffer(&device_ptr, external, &buf_desc);
		print_result(&api, "cuExternalMemoryGetMappedBuffer", result);
		if (result == CUDA_SUCCESS) {
			gpu_read_and_verify(&api, function,
					    "GPU read via imported dma-buf",
					    device_ptr, expected);
			api.mem_free(device_ptr);
		}
		api.destroy_external(external);
	}

	/*
	 * Control: this is not a direct mapping.  It proves whether the supported
	 * CUDA protected transfer path can consume bytes from the same CPU mapping.
	 */
	result = api.mem_alloc(&copy_ptr, sizeof(expected));
	print_result(&api, "cuMemAlloc(copy control)", result);
	if (result == CUDA_SUCCESS) {
		result = api.memcpy_htod(copy_ptr, mapping, sizeof(expected));
		print_result(&api, "cuMemcpyHtoD(from shared mmap)", result);
		if (result == CUDA_SUCCESS)
			gpu_read_and_verify(&api, function,
					    "GPU read after protected CUDA copy",
					    copy_ptr, expected);
		api.mem_free(copy_ptr);
	}

out:
	if (anonymous != MAP_FAILED)
		munmap(anonymous, size);
	if (module)
		api.module_unload(module);
	if (context)
		api.primary_release(device);
	if (import_fd >= 0)
		close(import_fd);
	dlclose(lib);
	munmap(mapping, size);
	close(alloc.fd);
	close(cvm_fd);
	return 0;
}
