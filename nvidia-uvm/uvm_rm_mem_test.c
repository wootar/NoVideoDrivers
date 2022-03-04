/*******************************************************************************
    Copyright (c) 2015-2021 NVIDIA Corporation

    Permission is hereby granted, free of charge, to any person obtaining a copy
    of this software and associated documentation files (the "Software"), to
    deal in the Software without restriction, including without limitation the
    rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
    sell copies of the Software, and to permit persons to whom the Software is
    furnished to do so, subject to the following conditions:

        The above copyright notice and this permission notice shall be
        included in all copies or substantial portions of the Software.

    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
    IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
    FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
    THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
    LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
    FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
    DEALINGS IN THE SOFTWARE.

*******************************************************************************/

#include "uvm_rm_mem.h"
#include "uvm_test.h"
#include "uvm_test_ioctl.h"
#include "uvm_va_space.h"
#include "uvm_kvmalloc.h"

static NV_STATUS map_cpu(uvm_rm_mem_t *rm_mem)
{
    void *cpu_va;

    // Map
    TEST_NV_CHECK_RET(uvm_rm_mem_map_cpu(rm_mem));
    TEST_CHECK_RET(uvm_rm_mem_mapped_on_cpu(rm_mem));

    // Mapping if already mapped is OK
    TEST_NV_CHECK_RET(uvm_rm_mem_map_cpu(rm_mem));

    // Unmap
    uvm_rm_mem_unmap_cpu(rm_mem);
    // Unmapping already unmapped also OK
    uvm_rm_mem_unmap_cpu(rm_mem);

    // Map again
    TEST_NV_CHECK_RET(uvm_rm_mem_map_cpu(rm_mem));

    cpu_va = uvm_rm_mem_get_cpu_va(rm_mem);
    TEST_CHECK_RET(cpu_va != NULL);

    // Check that the CPU VA is writable.
    // memset and memcpy might cause alignment faults on aarch64.
    // See Bug 2668765 for more details. Since this is a test ioctl and
    // therefore not a critical performance path, it's safe to use memset_io on
    // all platforms.
    memset_io(cpu_va, 0, rm_mem->size);

    return NV_OK;
}

static NV_STATUS map_gpu_owner(uvm_rm_mem_t *rm_mem)
{
    uvm_gpu_t *gpu = rm_mem->gpu_owner;

    // The memory should have been automatically mapped in the GPU owner
    TEST_CHECK_RET(uvm_rm_mem_mapped_on_gpu(rm_mem, gpu));

    // In SR-IOV heavy, there are two VA spaces per GPU, so there are two
    // mappings for a single rm_mem object on a GPU, even if the memory is
    // located in vidmem.
    TEST_CHECK_RET(uvm_rm_mem_mapped_on_gpu_proxy(rm_mem, gpu) == uvm_gpu_uses_proxy_channel_pool(gpu));

    // Explicitly mapping or unmapping to the GPU that owns the allocation is
    // not allowed, so the testing related to GPU owners is simpler than that of
    // other GPUs.
    return NV_OK;
}

static NV_STATUS map_other_gpus(uvm_rm_mem_t *rm_mem, uvm_va_space_t *va_space)
{
    uvm_gpu_t *gpu_owner = rm_mem->gpu_owner;
    uvm_gpu_t *gpu;

    for_each_va_space_gpu(gpu, va_space) {
        if (gpu == gpu_owner)
            continue;

        TEST_NV_CHECK_RET(uvm_rm_mem_map_gpu(rm_mem, gpu));
        TEST_CHECK_RET(uvm_rm_mem_mapped_on_gpu(rm_mem, gpu));

        // Mappings are not ref counted, so additional map calls are no-ops
        TEST_NV_CHECK_RET(uvm_rm_mem_map_gpu(rm_mem, gpu));

        // The previous GPU map calls added mappings to the proxy VA space
        // when in SR-IOV heavy mode
        TEST_CHECK_RET(uvm_rm_mem_mapped_on_gpu_proxy(rm_mem, gpu) == uvm_gpu_uses_proxy_channel_pool(gpu));

        // Unmapping removes all mappings
        uvm_rm_mem_unmap_gpu(rm_mem, gpu);
        TEST_CHECK_RET(!uvm_rm_mem_mapped_on_gpu(rm_mem, gpu));
        TEST_CHECK_RET(!uvm_rm_mem_mapped_on_gpu_proxy(rm_mem, gpu));

        // Additional unmappings are no-ops
        uvm_rm_mem_unmap_gpu(rm_mem, gpu);
        TEST_CHECK_RET(!uvm_rm_mem_mapped_on_gpu(rm_mem, gpu));
        TEST_CHECK_RET(!uvm_rm_mem_mapped_on_gpu_proxy(rm_mem, gpu));

        // Subsequent mappings should behave as they did in the beginning.
        TEST_NV_CHECK_RET(uvm_rm_mem_map_gpu(rm_mem, gpu));
        TEST_CHECK_RET(uvm_rm_mem_mapped_on_gpu(rm_mem, gpu));

        TEST_CHECK_RET(uvm_rm_mem_mapped_on_gpu_proxy(rm_mem, gpu) == uvm_gpu_uses_proxy_channel_pool(gpu));
    }

    return NV_OK;
}

static NV_STATUS test_all_gpus_in_va(uvm_va_space_t *va_space)
{
    uvm_gpu_t *gpu;
    uvm_rm_mem_t *rm_mem = NULL;
    NV_STATUS status = NV_OK;

    // Create allocations of these types
    static const uvm_rm_mem_type_t mem_types[] = { UVM_RM_MEM_TYPE_SYS, UVM_RM_MEM_TYPE_GPU };
    // Create allocations of these sizes
    static const size_t sizes[] = {1, 4, 16, 128, 1024, 4096, 1024 * 1024, 4 * 1024 * 1024};

    uvm_assert_rwsem_locked(&va_space->lock);

    TEST_CHECK_RET(!uvm_processor_mask_empty(&va_space->registered_gpus));

    for_each_va_space_gpu(gpu, va_space) {
        int i, j;

        for (i = 0; i < ARRAY_SIZE(sizes); ++i) {
            for (j = 0; j < ARRAY_SIZE(mem_types); ++j) {

                // Create an allocation in the GPU's address space
                TEST_NV_CHECK_RET(uvm_rm_mem_alloc(gpu, mem_types[j], sizes[i], &rm_mem));

                // Test CPU mappings
                TEST_NV_CHECK_GOTO(map_cpu(rm_mem), error);

                // Test mappings in the GPU owning the allocation
                TEST_NV_CHECK_GOTO(map_gpu_owner(rm_mem), error);

                // For sysmem allocations, test mappings on all other GPUs
                if (rm_mem->type == UVM_RM_MEM_TYPE_SYS)
                    TEST_NV_CHECK_GOTO(map_other_gpus(rm_mem, va_space), error);

                uvm_rm_mem_free(rm_mem);
            }
        }
    }

    return NV_OK;

error:
    uvm_rm_mem_free(rm_mem);

    return status;
}

NV_STATUS uvm_test_rm_mem_sanity(UVM_TEST_RM_MEM_SANITY_PARAMS *params, struct file *filp)
{
    NV_STATUS status;
    uvm_va_space_t *va_space = uvm_va_space_get(filp);

    uvm_va_space_down_read_rm(va_space);

    status = test_all_gpus_in_va(va_space);

    uvm_va_space_up_read_rm(va_space);

    return status;
}
