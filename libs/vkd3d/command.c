/*
 * Copyright 2016 Józef Kucia for CodeWeavers
 * Copyright 2016 Henri Verbeet for CodeWeavers
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include "vkd3d_private.h"

HRESULT vkd3d_queue_create(struct d3d12_device *device,
        uint32_t family_index, const VkQueueFamilyProperties *properties, struct vkd3d_queue **queue)
{
    const struct vkd3d_vk_device_procs *vk_procs = &device->vk_procs;
    struct vkd3d_queue *object;
    int rc;

    if (!(object = vkd3d_malloc(sizeof(*object))))
        return E_OUTOFMEMORY;

    if ((rc = pthread_mutex_init(&object->mutex, NULL)))
    {
        ERR("Failed to initialize mutex, error %d.\n", rc);
        vkd3d_free(object);
        return hresult_from_errno(rc);
    }

    object->vk_family_index = family_index;
    object->vk_queue_flags = properties->queueFlags;
    object->timestamp_bits = properties->timestampValidBits;

    VK_CALL(vkGetDeviceQueue(device->vk_device, family_index, 0, &object->vk_queue));

    TRACE("Created queue %p for queue family index %u.\n", object, family_index);

    *queue = object;

    return S_OK;
}

void vkd3d_queue_destroy(struct vkd3d_queue *queue)
{
    pthread_mutex_destroy(&queue->mutex);
    vkd3d_free(queue);
}

static VkQueue vkd3d_queue_acquire(struct vkd3d_queue *queue)
{
    int rc;

    TRACE("queue %p.\n", queue);

    if ((rc = pthread_mutex_lock(&queue->mutex)))
    {
        ERR("Failed to lock mutex, error %d.\n", rc);
        return VK_NULL_HANDLE;
    }

    assert(queue->vk_queue);
    return queue->vk_queue;
}

static void vkd3d_queue_release(struct vkd3d_queue *queue)
{
    TRACE("queue %p.\n", queue);

    pthread_mutex_unlock(&queue->mutex);
}

/* Fence worker thread */
static HRESULT vkd3d_enqueue_gpu_fence(struct vkd3d_fence_worker *worker,
        VkFence vk_fence, ID3D12Fence *fence, UINT64 value)
{
    int rc;

    TRACE("worker %p, fence %p, value %#"PRIx64".\n", worker, fence, value);

    if ((rc = pthread_mutex_lock(&worker->mutex)))
    {
        ERR("Failed to lock mutex, error %d.\n", rc);
        return hresult_from_errno(rc);
    }

    if (!vkd3d_array_reserve((void **)&worker->vk_fences, &worker->vk_fences_size,
            worker->fence_count + 1, sizeof(*worker->vk_fences)))
    {
        ERR("Failed to add GPU fence.\n");
        pthread_mutex_unlock(&worker->mutex);
        return E_OUTOFMEMORY;
    }
    if (!vkd3d_array_reserve((void **)&worker->fences, &worker->fences_size,
            worker->fence_count + 1, sizeof(*worker->fences)))
    {
        ERR("Failed to add GPU fence.\n");
        pthread_mutex_unlock(&worker->mutex);
        return E_OUTOFMEMORY;
    }

    worker->vk_fences[worker->fence_count] = vk_fence;
    worker->fences[worker->fence_count].fence = fence;
    worker->fences[worker->fence_count].value = value;
    ++worker->fence_count;

    pthread_cond_signal(&worker->cond);
    pthread_mutex_unlock(&worker->mutex);

    return S_OK;
}

static void vkd3d_fence_worker_remove_fence(struct vkd3d_fence_worker *worker, ID3D12Fence *fence)
{
    struct d3d12_device *device = worker->device;
    const struct vkd3d_vk_device_procs *vk_procs = &device->vk_procs;
    unsigned int i, j;
    int rc;

    if ((rc = pthread_mutex_lock(&worker->mutex)))
    {
        ERR("Failed to lock mutex, error %d.\n", rc);
        return;
    }

    for (i = 0, j = 0; i < worker->fence_count; ++i)
    {
        if (worker->fences[i].fence == fence)
        {
            VK_CALL(vkDestroyFence(device->vk_device, worker->vk_fences[i], NULL));
            continue;
        }

        if (i != j)
        {
            worker->vk_fences[j] = worker->vk_fences[i];
            worker->fences[j] = worker->fences[i];
        }
        ++j;
    }
    worker->fence_count = j;

    pthread_mutex_unlock(&worker->mutex);
}

static void vkd3d_wait_for_gpu_fences(struct vkd3d_fence_worker *worker)
{
    struct d3d12_device *device = worker->device;
    const struct vkd3d_vk_device_procs *vk_procs = &device->vk_procs;
    unsigned int i, j;
    HRESULT hr;
    int vr;

    if (!worker->fence_count)
        return;

    vr = VK_CALL(vkWaitForFences(device->vk_device,
            worker->fence_count, worker->vk_fences, VK_FALSE, 0));
    if (vr == VK_TIMEOUT)
        return;
    if (vr != VK_SUCCESS)
    {
        ERR("Failed to wait for Vulkan fences, vr %d.\n", vr);
        return;
    }

    for (i = 0, j = 0; i < worker->fence_count; ++i)
    {
        if (!(vr = VK_CALL(vkGetFenceStatus(device->vk_device, worker->vk_fences[i]))))
        {
            struct vkd3d_waiting_fence *current = &worker->fences[i];
            if (FAILED(hr = ID3D12Fence_Signal(current->fence, current->value)))
                ERR("Failed to signal D3D12 fence, hr %d.\n", hr);
            VK_CALL(vkDestroyFence(device->vk_device, worker->vk_fences[i], NULL));
            continue;
        }

        if (vr != VK_NOT_READY)
            ERR("Failed to get Vulkan fence status, vr %d.\n", vr);

        if (i != j)
        {
            worker->vk_fences[j] = worker->vk_fences[i];
            worker->fences[j] = worker->fences[i];
        }
        ++j;
    }
    worker->fence_count = j;
}

static void *vkd3d_fence_worker_main(void *arg)
{
    struct vkd3d_fence_worker *worker = arg;
    int rc;

    vkd3d_set_thread_name("vkd3d_worker");

    for (;;)
    {
        if ((rc = pthread_mutex_lock(&worker->mutex)))
        {
            ERR("Failed to lock mutex, error %d.\n", rc);
            return NULL;
        }

        if (worker->should_exit && !worker->fence_count)
        {
            pthread_mutex_unlock(&worker->mutex);
            break;
        }

        if (!worker->fence_count)
        {
            if ((rc = pthread_cond_wait(&worker->cond, &worker->mutex)))
            {
                ERR("Failed to wait on condition variable, error %d.\n", rc);
                pthread_mutex_unlock(&worker->mutex);
                return NULL;
            }
        }

        vkd3d_wait_for_gpu_fences(worker);

        pthread_mutex_unlock(&worker->mutex);
    }

    return NULL;
}

HRESULT vkd3d_fence_worker_start(struct vkd3d_fence_worker *worker,
        struct d3d12_device *device)
{
    int rc;

    TRACE("worker %p.\n", worker);

    worker->should_exit = false;
    worker->device = device;

    worker->fence_count = 0;

    worker->vk_fences = NULL;
    worker->vk_fences_size = 0;
    worker->fences = NULL;
    worker->fences_size = 0;

    if ((rc = pthread_mutex_init(&worker->mutex, NULL)))
    {
        ERR("Failed to initialize mutex, error %d.\n", rc);
        return hresult_from_errno(rc);
    }

    if ((rc = pthread_cond_init(&worker->cond, NULL)))
    {
        ERR("Failed to initialize condition variable, error %d.\n", rc);
        pthread_mutex_destroy(&worker->mutex);
        return hresult_from_errno(rc);
    }

    if (device->create_thread)
    {
        if (!(worker->u.handle = device->create_thread(vkd3d_fence_worker_main, worker)))
        {
            ERR("Failed to create fence worker thread.\n");
            pthread_mutex_destroy(&worker->mutex);
            pthread_cond_destroy(&worker->cond);
            return E_FAIL;
        }

        return S_OK;
    }

    if ((rc = pthread_create(&worker->u.thread, NULL, vkd3d_fence_worker_main, worker)))
    {
        ERR("Failed to create fence worker thread, error %d.\n", rc);
        pthread_mutex_destroy(&worker->mutex);
        pthread_cond_destroy(&worker->cond);
        return hresult_from_errno(rc);
    }

    return S_OK;
}

HRESULT vkd3d_fence_worker_stop(struct vkd3d_fence_worker *worker,
        struct d3d12_device *device)
{
    HRESULT hr;
    int rc;

    TRACE("worker %p.\n", worker);

    if ((rc = pthread_mutex_lock(&worker->mutex)))
    {
        ERR("Failed to lock mutex, error %d.\n", rc);
        return hresult_from_errno(rc);
    }

    worker->should_exit = true;
    pthread_cond_signal(&worker->cond);

    pthread_mutex_unlock(&worker->mutex);

    if (device->join_thread)
    {
        if (FAILED(hr = device->join_thread(worker->u.handle)))
        {
            ERR("Failed to join fence worker thread, hr %#x.\n", hr);
            return hr;
        }
    }
    else
    {
        if ((rc = pthread_join(worker->u.thread, NULL)))
        {
            ERR("Failed to join fence worker thread, error %d.\n", rc);
            return hresult_from_errno(rc);
        }
    }

    pthread_mutex_destroy(&worker->mutex);
    pthread_cond_destroy(&worker->cond);

    vkd3d_free(worker->vk_fences);
    vkd3d_free(worker->fences);

    return S_OK;
}

/* ID3D12Fence */
static struct d3d12_fence *impl_from_ID3D12Fence(ID3D12Fence *iface)
{
    return CONTAINING_RECORD(iface, struct d3d12_fence, ID3D12Fence_iface);
}

static HRESULT STDMETHODCALLTYPE d3d12_fence_QueryInterface(ID3D12Fence *iface,
        REFIID riid, void **object)
{
    TRACE("iface %p, riid %s, object %p.\n", iface, debugstr_guid(riid), object);

    if (IsEqualGUID(riid, &IID_ID3D12Fence)
            || IsEqualGUID(riid, &IID_ID3D12Pageable)
            || IsEqualGUID(riid, &IID_ID3D12DeviceChild)
            || IsEqualGUID(riid, &IID_ID3D12Object)
            || IsEqualGUID(riid, &IID_IUnknown))
    {
        ID3D12Fence_AddRef(iface);
        *object = iface;
        return S_OK;
    }

    WARN("%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid(riid));

    *object = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE d3d12_fence_AddRef(ID3D12Fence *iface)
{
    struct d3d12_fence *fence = impl_from_ID3D12Fence(iface);
    ULONG refcount = InterlockedIncrement(&fence->refcount);

    TRACE("%p increasing refcount to %u.\n", fence, refcount);

    return refcount;
}

static ULONG STDMETHODCALLTYPE d3d12_fence_Release(ID3D12Fence *iface)
{
    struct d3d12_fence *fence = impl_from_ID3D12Fence(iface);
    ULONG refcount = InterlockedDecrement(&fence->refcount);
    int rc;

    TRACE("%p decreasing refcount to %u.\n", fence, refcount);

    if (!refcount)
    {
        struct d3d12_device *device = fence->device;

        vkd3d_private_store_destroy(&fence->private_store);

        vkd3d_fence_worker_remove_fence(&device->fence_worker, iface);

        vkd3d_free(fence->events);
        if ((rc = pthread_mutex_destroy(&fence->mutex)))
            ERR("Failed to destroy mutex, error %d.\n", rc);
        vkd3d_free(fence);

        ID3D12Device_Release(&device->ID3D12Device_iface);
    }

    return refcount;
}

static HRESULT STDMETHODCALLTYPE d3d12_fence_GetPrivateData(ID3D12Fence *iface,
        REFGUID guid, UINT *data_size, void *data)
{
    struct d3d12_fence *fence = impl_from_ID3D12Fence(iface);

    TRACE("iface %p, guid %s, data_size %p, data %p.\n",
            iface, debugstr_guid(guid), data_size, data);

    return vkd3d_get_private_data(&fence->private_store, guid, data_size, data);
}

static HRESULT STDMETHODCALLTYPE d3d12_fence_SetPrivateData(ID3D12Fence *iface,
        REFGUID guid, UINT data_size, const void *data)
{
    struct d3d12_fence *fence = impl_from_ID3D12Fence(iface);

    TRACE("iface %p, guid %s, data_size %u, data %p.\n",
            iface, debugstr_guid(guid), data_size, data);

    return vkd3d_set_private_data(&fence->private_store, guid, data_size, data);
}

static HRESULT STDMETHODCALLTYPE d3d12_fence_SetPrivateDataInterface(ID3D12Fence *iface,
        REFGUID guid, const IUnknown *data)
{
    struct d3d12_fence *fence = impl_from_ID3D12Fence(iface);

    TRACE("iface %p, guid %s, data %p.\n", iface, debugstr_guid(guid), data);

    return vkd3d_set_private_data_interface(&fence->private_store, guid, data);
}

static HRESULT STDMETHODCALLTYPE d3d12_fence_SetName(ID3D12Fence *iface, const WCHAR *name)
{
    struct d3d12_fence *fence = impl_from_ID3D12Fence(iface);

    FIXME("iface %p, name %s stub!\n", iface, debugstr_w(name, fence->device->wchar_size));

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE d3d12_fence_GetDevice(ID3D12Fence *iface,
        REFIID riid, void **device)
{
    struct d3d12_fence *fence = impl_from_ID3D12Fence(iface);

    TRACE("iface %p, riid %s, device %p.\n", iface, debugstr_guid(riid), device);

    return ID3D12Device_QueryInterface(&fence->device->ID3D12Device_iface, riid, device);
}

static UINT64 STDMETHODCALLTYPE d3d12_fence_GetCompletedValue(ID3D12Fence *iface)
{
    struct d3d12_fence *fence = impl_from_ID3D12Fence(iface);
    UINT64 completed_value;
    int rc;

    TRACE("iface %p.\n", iface);

    if ((rc = pthread_mutex_lock(&fence->mutex)))
    {
        ERR("Failed to lock mutex, error %d.\n", rc);
        return 0;
    }
    completed_value = fence->value;
    pthread_mutex_unlock(&fence->mutex);
    return completed_value;
}

static HRESULT STDMETHODCALLTYPE d3d12_fence_SetEventOnCompletion(ID3D12Fence *iface,
        UINT64 value, HANDLE event)
{
    struct d3d12_fence *fence = impl_from_ID3D12Fence(iface);
    unsigned int i;
    int rc;

    TRACE("iface %p, value %#"PRIx64", event %p.\n", iface, value, event);

    if ((rc = pthread_mutex_lock(&fence->mutex)))
    {
        ERR("Failed to lock mutex, error %d.\n", rc);
        return hresult_from_errno(rc);
    }

    if (value <= fence->value)
    {
        fence->device->signal_event(event);
        pthread_mutex_unlock(&fence->mutex);
        return S_OK;
    }

    for (i = 0; i < fence->event_count; ++i)
    {
        struct vkd3d_waiting_event *current = &fence->events[i];
        if (current->value == value && current->event == event)
        {
            WARN("Event completion for (%p, %#"PRIx64") is already in the list.\n",
                    event, value);
            pthread_mutex_unlock(&fence->mutex);
            return S_OK;
        }
    }

    if (!vkd3d_array_reserve((void **)&fence->events, &fence->events_size,
            fence->event_count + 1, sizeof(*fence->events)))
    {
        WARN("Failed to add event.\n");
        pthread_mutex_unlock(&fence->mutex);
        return E_OUTOFMEMORY;
    }

    fence->events[fence->event_count].value = value;
    fence->events[fence->event_count].event = event;
    ++fence->event_count;

    pthread_mutex_unlock(&fence->mutex);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE d3d12_fence_Signal(ID3D12Fence *iface, UINT64 value)
{
    struct d3d12_fence *fence = impl_from_ID3D12Fence(iface);
    unsigned int i, j;
    int rc;

    TRACE("iface %p, value %#"PRIx64".\n", iface, value);

    if ((rc = pthread_mutex_lock(&fence->mutex)))
    {
        ERR("Failed to lock mutex, error %d.\n", rc);
        return hresult_from_errno(rc);
    }

    fence->value = value;

    for (i = 0, j = 0; i < fence->event_count; ++i)
    {
        struct vkd3d_waiting_event *current = &fence->events[i];

        if (current->value <= value)
        {
            fence->device->signal_event(current->event);
        }
        else
        {
            if (i != j)
                fence->events[j] = *current;
            ++j;
        }
    }
    fence->event_count = j;

    pthread_mutex_unlock(&fence->mutex);

    return S_OK;
}

static const struct ID3D12FenceVtbl d3d12_fence_vtbl =
{
    /* IUnknown methods */
    d3d12_fence_QueryInterface,
    d3d12_fence_AddRef,
    d3d12_fence_Release,
    /* ID3D12Object methods */
    d3d12_fence_GetPrivateData,
    d3d12_fence_SetPrivateData,
    d3d12_fence_SetPrivateDataInterface,
    d3d12_fence_SetName,
    /* ID3D12DeviceChild methods */
    d3d12_fence_GetDevice,
    /* ID3D12Fence methods */
    d3d12_fence_GetCompletedValue,
    d3d12_fence_SetEventOnCompletion,
    d3d12_fence_Signal,
};

static HRESULT d3d12_fence_init(struct d3d12_fence *fence, struct d3d12_device *device,
        UINT64 initial_value, D3D12_FENCE_FLAGS flags)
{
    HRESULT hr;
    int rc;

    fence->ID3D12Fence_iface.lpVtbl = &d3d12_fence_vtbl;
    fence->refcount = 1;

    fence->value = initial_value;

    if ((rc = pthread_mutex_init(&fence->mutex, NULL)))
    {
        ERR("Failed to initialize mutex, error %d.\n", rc);
        return hresult_from_errno(rc);
    }

    if (flags)
        FIXME("Ignoring flags %#x.\n", flags);

    fence->events = NULL;
    fence->events_size = 0;
    fence->event_count = 0;

    if (FAILED(hr = vkd3d_private_store_init(&fence->private_store)))
    {
        pthread_mutex_destroy(&fence->mutex);
        return hr;
    }

    fence->device = device;
    ID3D12Device_AddRef(&device->ID3D12Device_iface);

    return S_OK;
}

HRESULT d3d12_fence_create(struct d3d12_device *device,
        UINT64 initial_value, D3D12_FENCE_FLAGS flags, struct d3d12_fence **fence)
{
    struct d3d12_fence *object;

    if (!(object = vkd3d_malloc(sizeof(*object))))
        return E_OUTOFMEMORY;

    d3d12_fence_init(object, device, initial_value, flags);

    TRACE("Created fence %p.\n", object);

    *fence = object;

    return S_OK;
}

/* Command buffers */
static HRESULT d3d12_command_list_begin_command_buffer(struct d3d12_command_list *list)
{
    struct d3d12_device *device = list->device;
    const struct vkd3d_vk_device_procs *vk_procs = &device->vk_procs;
    VkCommandBufferBeginInfo begin_info;
    VkResult vr;

    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.pNext = NULL;
    begin_info.flags = 0;
    begin_info.pInheritanceInfo = NULL;

    if ((vr = VK_CALL(vkBeginCommandBuffer(list->vk_command_buffer, &begin_info))) < 0)
    {
        WARN("Failed to begin command buffer, vr %d.\n", vr);
        return hresult_from_vk_result(vr);
    }

    list->is_recording = true;
    list->is_valid = true;

    return S_OK;
}

static HRESULT d3d12_command_allocator_allocate_command_buffer(struct d3d12_command_allocator *allocator,
        struct d3d12_command_list *list)
{
    struct d3d12_device *device = allocator->device;
    const struct vkd3d_vk_device_procs *vk_procs = &device->vk_procs;
    VkCommandBufferAllocateInfo command_buffer_info;
    VkResult vr;
    HRESULT hr;

    TRACE("allocator %p, list %p.\n", allocator, list);

    if (allocator->current_command_list)
    {
        WARN("Command allocator is already in use.\n");
        return E_INVALIDARG;
    }

    command_buffer_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    command_buffer_info.pNext = NULL;
    command_buffer_info.commandPool = allocator->vk_command_pool;
    command_buffer_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    command_buffer_info.commandBufferCount = 1;

    if ((vr = VK_CALL(vkAllocateCommandBuffers(device->vk_device, &command_buffer_info,
            &list->vk_command_buffer))) < 0)
    {
        WARN("Failed to allocate Vulkan command buffer, vr %d.\n", vr);
        return hresult_from_vk_result(vr);
    }

    list->vk_queue_flags = allocator->vk_queue_flags;

    if (FAILED(hr = d3d12_command_list_begin_command_buffer(list)))
    {
        VK_CALL(vkFreeCommandBuffers(device->vk_device, allocator->vk_command_pool,
                1, &list->vk_command_buffer));
        return hr;
    }

    allocator->current_command_list = list;

    return S_OK;
}

static void d3d12_command_allocator_free_command_buffer(struct d3d12_command_allocator *allocator,
        struct d3d12_command_list *list)
{
    struct d3d12_device *device = allocator->device;
    const struct vkd3d_vk_device_procs *vk_procs = &device->vk_procs;

    TRACE("allocator %p, list %p.\n", allocator, list);

    if (allocator->current_command_list == list)
        allocator->current_command_list = NULL;

    if (!vkd3d_array_reserve((void **)&allocator->command_buffers, &allocator->command_buffers_size,
            allocator->command_buffer_count + 1, sizeof(*allocator->command_buffers)))
    {
        WARN("Failed to add command buffer.\n");
        VK_CALL(vkFreeCommandBuffers(device->vk_device, allocator->vk_command_pool,
                1, &list->vk_command_buffer));
        return;
    }

    allocator->command_buffers[allocator->command_buffer_count++] = list->vk_command_buffer;
}

static bool d3d12_command_allocator_add_render_pass(struct d3d12_command_allocator *allocator, VkRenderPass pass)
{
    if (!vkd3d_array_reserve((void **)&allocator->passes, &allocator->passes_size,
            allocator->pass_count + 1, sizeof(*allocator->passes)))
        return false;

    allocator->passes[allocator->pass_count++] = pass;

    return true;
}

static bool d3d12_command_allocator_add_framebuffer(struct d3d12_command_allocator *allocator,
        VkFramebuffer framebuffer)
{
    if (!vkd3d_array_reserve((void **)&allocator->framebuffers, &allocator->framebuffers_size,
            allocator->framebuffer_count + 1, sizeof(*allocator->framebuffers)))
        return false;

    allocator->framebuffers[allocator->framebuffer_count++] = framebuffer;

    return true;
}

static bool d3d12_command_allocator_add_descriptor_pool(struct d3d12_command_allocator *allocator,
        VkDescriptorPool pool)
{
    if (!vkd3d_array_reserve((void **)&allocator->descriptor_pools, &allocator->descriptor_pools_size,
            allocator->descriptor_pool_count + 1, sizeof(*allocator->descriptor_pools)))
        return false;

    allocator->descriptor_pools[allocator->descriptor_pool_count++] = pool;

    return true;
}

static bool d3d12_command_allocator_add_view(struct d3d12_command_allocator *allocator,
        struct vkd3d_view *view)
{
    if (!vkd3d_array_reserve((void **)&allocator->views, &allocator->views_size,
            allocator->view_count + 1, sizeof(*allocator->views)))
        return false;

    vkd3d_view_incref(view);
    allocator->views[allocator->view_count++] = view;

    return true;
}

static bool d3d12_command_allocator_add_buffer_view(struct d3d12_command_allocator *allocator,
        VkBufferView view)
{
    if (!vkd3d_array_reserve((void **)&allocator->buffer_views, &allocator->buffer_views_size,
            allocator->buffer_view_count + 1, sizeof(*allocator->buffer_views)))
        return false;

    allocator->buffer_views[allocator->buffer_view_count++] = view;

    return true;
}

static bool d3d12_command_allocator_add_transfer_buffer(struct d3d12_command_allocator *allocator,
        const struct vkd3d_buffer *buffer)
{
    if (!vkd3d_array_reserve((void **)&allocator->transfer_buffers, &allocator->transfer_buffers_size,
            allocator->transfer_buffer_count + 1, sizeof(*allocator->transfer_buffers)))
        return false;

    allocator->transfer_buffers[allocator->transfer_buffer_count++] = *buffer;

    return true;
}

static VkDescriptorPool d3d12_command_allocator_allocate_descriptor_pool(
        struct d3d12_command_allocator *allocator)
{
    static const VkDescriptorPoolSize pool_sizes[] =
    {
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1024},
        {VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 1024},
        {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1024},
        {VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, 1024},
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1024},
        {VK_DESCRIPTOR_TYPE_SAMPLER, 1024},
    };
    struct d3d12_device *device = allocator->device;
    const struct vkd3d_vk_device_procs *vk_procs = &device->vk_procs;
    struct VkDescriptorPoolCreateInfo pool_desc;
    VkDevice vk_device = device->vk_device;
    VkDescriptorPool vk_pool;
    VkResult vr;

    if (allocator->free_descriptor_pool_count > 0)
    {
        vk_pool = allocator->free_descriptor_pools[allocator->free_descriptor_pool_count - 1];
        allocator->free_descriptor_pools[allocator->free_descriptor_pool_count - 1] = VK_NULL_HANDLE;
        --allocator->free_descriptor_pool_count;
    }
    else
    {
        pool_desc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pool_desc.pNext = NULL;
        pool_desc.flags = 0;
        pool_desc.maxSets = 512;
        pool_desc.poolSizeCount = ARRAY_SIZE(pool_sizes);
        pool_desc.pPoolSizes = pool_sizes;
        if ((vr = VK_CALL(vkCreateDescriptorPool(vk_device, &pool_desc, NULL, &vk_pool))) < 0)
        {
            ERR("Failed to create descriptor pool, vr %d.\n", vr);
            return VK_NULL_HANDLE;
        }
    }

    if (!(d3d12_command_allocator_add_descriptor_pool(allocator, vk_pool)))
    {
        ERR("Failed to add descriptor pool.\n");
        VK_CALL(vkDestroyDescriptorPool(vk_device, vk_pool, NULL));
        return VK_NULL_HANDLE;
    }

    return vk_pool;
}

static VkDescriptorSet d3d12_command_allocator_allocate_descriptor_set(
        struct d3d12_command_allocator *allocator, VkDescriptorSetLayout vk_set_layout)
{
    struct d3d12_device *device = allocator->device;
    const struct vkd3d_vk_device_procs *vk_procs = &device->vk_procs;
    struct VkDescriptorSetAllocateInfo set_desc;
    VkDevice vk_device = device->vk_device;
    VkDescriptorSet vk_descriptor_set;
    VkResult vr;

    if (!allocator->vk_descriptor_pool)
        allocator->vk_descriptor_pool = d3d12_command_allocator_allocate_descriptor_pool(allocator);
    if (!allocator->vk_descriptor_pool)
        return VK_NULL_HANDLE;

    set_desc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    set_desc.pNext = NULL;
    set_desc.descriptorPool = allocator->vk_descriptor_pool;
    set_desc.descriptorSetCount = 1;
    set_desc.pSetLayouts = &vk_set_layout;
    if ((vr = VK_CALL(vkAllocateDescriptorSets(vk_device, &set_desc, &vk_descriptor_set))) >= 0)
        return vk_descriptor_set;

    allocator->vk_descriptor_pool = VK_NULL_HANDLE;
    if (vr == VK_ERROR_FRAGMENTED_POOL || vr == VK_ERROR_OUT_OF_POOL_MEMORY_KHR)
        allocator->vk_descriptor_pool = d3d12_command_allocator_allocate_descriptor_pool(allocator);
    if (!allocator->vk_descriptor_pool)
    {
        ERR("Failed to allocate descriptor set, vr %d.\n", vr);
        return VK_NULL_HANDLE;
    }

    set_desc.descriptorPool = allocator->vk_descriptor_pool;
    if ((vr = VK_CALL(vkAllocateDescriptorSets(vk_device, &set_desc, &vk_descriptor_set))) < 0)
    {
        FIXME("Failed to allocate descriptor set from a new pool, vr %d.\n", vr);
        return VK_NULL_HANDLE;
    }

    return vk_descriptor_set;
}

static void d3d12_command_list_allocator_destroyed(struct d3d12_command_list *list)
{
    TRACE("list %p.\n", list);

    list->allocator = NULL;
    list->vk_command_buffer = VK_NULL_HANDLE;
}

static void vkd3d_buffer_destroy(struct vkd3d_buffer *buffer, struct d3d12_device *device)
{
    const struct vkd3d_vk_device_procs *vk_procs = &device->vk_procs;

    VK_CALL(vkFreeMemory(device->vk_device, buffer->vk_memory, NULL));
    VK_CALL(vkDestroyBuffer(device->vk_device, buffer->vk_buffer, NULL));
}

static void d3d12_command_allocator_free_resources(struct d3d12_command_allocator *allocator,
        bool keep_reusable_resources)
{
    struct d3d12_device *device = allocator->device;
    const struct vkd3d_vk_device_procs *vk_procs = &device->vk_procs;
    unsigned int i, j;

    allocator->vk_descriptor_pool = VK_NULL_HANDLE;

    if (keep_reusable_resources)
    {
        if (vkd3d_array_reserve((void **)&allocator->free_descriptor_pools,
                &allocator->free_descriptor_pools_size,
                allocator->free_descriptor_pool_count + allocator->descriptor_pool_count,
                sizeof(*allocator->free_descriptor_pools)))
        {
            for (i = 0, j = allocator->free_descriptor_pool_count; i < allocator->descriptor_pool_count; ++i, ++j)
            {
                VK_CALL(vkResetDescriptorPool(device->vk_device, allocator->descriptor_pools[i], 0));
                allocator->free_descriptor_pools[j] = allocator->descriptor_pools[i];
            }
            allocator->free_descriptor_pool_count += allocator->descriptor_pool_count;
            allocator->descriptor_pool_count = 0;
        }
    }
    else
    {
        for (i = 0; i < allocator->free_descriptor_pool_count; ++i)
        {
            VK_CALL(vkDestroyDescriptorPool(device->vk_device, allocator->free_descriptor_pools[i], NULL));
        }
        allocator->free_descriptor_pool_count = 0;
    }

    for (i = 0; i < allocator->transfer_buffer_count; ++i)
    {
        vkd3d_buffer_destroy(&allocator->transfer_buffers[i], device);
    }
    allocator->transfer_buffer_count = 0;

    for (i = 0; i < allocator->buffer_view_count; ++i)
    {
        VK_CALL(vkDestroyBufferView(device->vk_device, allocator->buffer_views[i], NULL));
    }
    allocator->buffer_view_count = 0;

    for (i = 0; i < allocator->view_count; ++i)
    {
        vkd3d_view_decref(allocator->views[i], device);
    }
    allocator->view_count = 0;

    for (i = 0; i < allocator->descriptor_pool_count; ++i)
    {
        VK_CALL(vkDestroyDescriptorPool(device->vk_device, allocator->descriptor_pools[i], NULL));
    }
    allocator->descriptor_pool_count = 0;

    for (i = 0; i < allocator->framebuffer_count; ++i)
    {
        VK_CALL(vkDestroyFramebuffer(device->vk_device, allocator->framebuffers[i], NULL));
    }
    allocator->framebuffer_count = 0;

    for (i = 0; i < allocator->pass_count; ++i)
    {
        VK_CALL(vkDestroyRenderPass(device->vk_device, allocator->passes[i], NULL));
    }
    allocator->pass_count = 0;
}

/* ID3D12CommandAllocator */
static inline struct d3d12_command_allocator *impl_from_ID3D12CommandAllocator(ID3D12CommandAllocator *iface)
{
    return CONTAINING_RECORD(iface, struct d3d12_command_allocator, ID3D12CommandAllocator_iface);
}

static HRESULT STDMETHODCALLTYPE d3d12_command_allocator_QueryInterface(ID3D12CommandAllocator *iface,
        REFIID riid, void **object)
{
    TRACE("iface %p, riid %s, object %p.\n", iface, debugstr_guid(riid), object);

    if (IsEqualGUID(riid, &IID_ID3D12CommandAllocator)
            || IsEqualGUID(riid, &IID_ID3D12Pageable)
            || IsEqualGUID(riid, &IID_ID3D12DeviceChild)
            || IsEqualGUID(riid, &IID_ID3D12Object)
            || IsEqualGUID(riid, &IID_IUnknown))
    {
        ID3D12CommandAllocator_AddRef(iface);
        *object = iface;
        return S_OK;
    }

    WARN("%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid(riid));

    *object = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE d3d12_command_allocator_AddRef(ID3D12CommandAllocator *iface)
{
    struct d3d12_command_allocator *allocator = impl_from_ID3D12CommandAllocator(iface);
    ULONG refcount = InterlockedIncrement(&allocator->refcount);

    TRACE("%p increasing refcount to %u.\n", allocator, refcount);

    return refcount;
}

static ULONG STDMETHODCALLTYPE d3d12_command_allocator_Release(ID3D12CommandAllocator *iface)
{
    struct d3d12_command_allocator *allocator = impl_from_ID3D12CommandAllocator(iface);
    ULONG refcount = InterlockedDecrement(&allocator->refcount);

    TRACE("%p decreasing refcount to %u.\n", allocator, refcount);

    if (!refcount)
    {
        struct d3d12_device *device = allocator->device;
        const struct vkd3d_vk_device_procs *vk_procs = &device->vk_procs;

        vkd3d_private_store_destroy(&allocator->private_store);

        if (allocator->current_command_list)
            d3d12_command_list_allocator_destroyed(allocator->current_command_list);

        d3d12_command_allocator_free_resources(allocator, false);
        vkd3d_free(allocator->transfer_buffers);
        vkd3d_free(allocator->buffer_views);
        vkd3d_free(allocator->views);
        vkd3d_free(allocator->descriptor_pools);
        vkd3d_free(allocator->free_descriptor_pools);
        vkd3d_free(allocator->framebuffers);
        vkd3d_free(allocator->passes);

        /* All command buffers are implicitly freed when a pool is destroyed. */
        vkd3d_free(allocator->command_buffers);
        VK_CALL(vkDestroyCommandPool(device->vk_device, allocator->vk_command_pool, NULL));

        vkd3d_free(allocator);

        ID3D12Device_Release(&device->ID3D12Device_iface);
    }

    return refcount;
}

static HRESULT STDMETHODCALLTYPE d3d12_command_allocator_GetPrivateData(ID3D12CommandAllocator *iface,
        REFGUID guid, UINT *data_size, void *data)
{
    struct d3d12_command_allocator *allocator = impl_from_ID3D12CommandAllocator(iface);

    TRACE("iface %p, guid %s, data_size %p, data %p.\n", iface, debugstr_guid(guid), data_size, data);

    return vkd3d_get_private_data(&allocator->private_store, guid, data_size, data);
}

static HRESULT STDMETHODCALLTYPE d3d12_command_allocator_SetPrivateData(ID3D12CommandAllocator *iface,
        REFGUID guid, UINT data_size, const void *data)
{
    struct d3d12_command_allocator *allocator = impl_from_ID3D12CommandAllocator(iface);

    TRACE("iface %p, guid %s, data_size %u, data %p.\n", iface, debugstr_guid(guid), data_size, data);

    return vkd3d_set_private_data(&allocator->private_store, guid, data_size, data);
}

static HRESULT STDMETHODCALLTYPE d3d12_command_allocator_SetPrivateDataInterface(ID3D12CommandAllocator *iface,
        REFGUID guid, const IUnknown *data)
{
    struct d3d12_command_allocator *allocator = impl_from_ID3D12CommandAllocator(iface);

    TRACE("iface %p, guid %s, data %p.\n", iface, debugstr_guid(guid), data);

    return vkd3d_set_private_data_interface(&allocator->private_store, guid, data);
}

static HRESULT STDMETHODCALLTYPE d3d12_command_allocator_SetName(ID3D12CommandAllocator *iface, const WCHAR *name)
{
    struct d3d12_command_allocator *allocator = impl_from_ID3D12CommandAllocator(iface);

    FIXME("iface %p, name %s stub!\n", iface, debugstr_w(name, allocator->device->wchar_size));

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE d3d12_command_allocator_GetDevice(ID3D12CommandAllocator *iface,
        REFIID riid, void **device)
{
    struct d3d12_command_allocator *allocator = impl_from_ID3D12CommandAllocator(iface);

    TRACE("iface %p, riid %s, device %p.\n", iface, debugstr_guid(riid), device);

    return ID3D12Device_QueryInterface(&allocator->device->ID3D12Device_iface, riid, device);
}

static HRESULT STDMETHODCALLTYPE d3d12_command_allocator_Reset(ID3D12CommandAllocator *iface)
{
    struct d3d12_command_allocator *allocator = impl_from_ID3D12CommandAllocator(iface);
    const struct vkd3d_vk_device_procs *vk_procs;
    struct d3d12_command_list *list;
    struct d3d12_device *device;
    VkResult vr;

    TRACE("iface %p.\n", iface);

    if ((list = allocator->current_command_list))
    {
        if (list->is_recording)
        {
            WARN("A command list using this allocator is in the recording state.\n");
            return E_FAIL;
        }

        TRACE("Resetting command list %p.\n", list);
    }

    device = allocator->device;
    vk_procs = &device->vk_procs;

    d3d12_command_allocator_free_resources(allocator, true);
    if (allocator->command_buffer_count)
    {
        VK_CALL(vkFreeCommandBuffers(device->vk_device, allocator->vk_command_pool,
                allocator->command_buffer_count, allocator->command_buffers));
        allocator->command_buffer_count = 0;
    }

    if ((vr = VK_CALL(vkResetCommandPool(device->vk_device, allocator->vk_command_pool,
            VK_COMMAND_POOL_RESET_RELEASE_RESOURCES_BIT))))
    {
        WARN("Resetting command pool failed, vr %d.\n", vr);
        return hresult_from_vk_result(vr);
    }

    return S_OK;
}

static const struct ID3D12CommandAllocatorVtbl d3d12_command_allocator_vtbl =
{
    /* IUnknown methods */
    d3d12_command_allocator_QueryInterface,
    d3d12_command_allocator_AddRef,
    d3d12_command_allocator_Release,
    /* ID3D12Object methods */
    d3d12_command_allocator_GetPrivateData,
    d3d12_command_allocator_SetPrivateData,
    d3d12_command_allocator_SetPrivateDataInterface,
    d3d12_command_allocator_SetName,
    /* ID3D12DeviceChild methods */
    d3d12_command_allocator_GetDevice,
    /* ID3D12CommandAllocator methods */
    d3d12_command_allocator_Reset,
};

static struct d3d12_command_allocator *unsafe_impl_from_ID3D12CommandAllocator(ID3D12CommandAllocator *iface)
{
    if (!iface)
        return NULL;
    assert(iface->lpVtbl == &d3d12_command_allocator_vtbl);
    return impl_from_ID3D12CommandAllocator(iface);
}

static struct vkd3d_queue *d3d12_device_get_vkd3d_queue(struct d3d12_device *device,
        D3D12_COMMAND_LIST_TYPE type)
{
    switch (type)
    {
        case D3D12_COMMAND_LIST_TYPE_DIRECT:
            return device->direct_queue;
        case D3D12_COMMAND_LIST_TYPE_COMPUTE:
            return device->compute_queue;
        case D3D12_COMMAND_LIST_TYPE_COPY:
            return device->copy_queue;
        default:
            FIXME("Unhandled command list type %#x.\n", type);
            return NULL;
    }
}

static HRESULT d3d12_command_allocator_init(struct d3d12_command_allocator *allocator,
        struct d3d12_device *device, D3D12_COMMAND_LIST_TYPE type)
{
    const struct vkd3d_vk_device_procs *vk_procs = &device->vk_procs;
    VkCommandPoolCreateInfo command_pool_info;
    struct vkd3d_queue *queue;
    VkResult vr;
    HRESULT hr;

    if (FAILED(hr = vkd3d_private_store_init(&allocator->private_store)))
        return hr;

    if (!(queue = d3d12_device_get_vkd3d_queue(device, type)))
        queue = device->direct_queue;

    allocator->ID3D12CommandAllocator_iface.lpVtbl = &d3d12_command_allocator_vtbl;
    allocator->refcount = 1;

    allocator->type = type;
    allocator->vk_queue_flags = queue->vk_queue_flags;

    command_pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    command_pool_info.pNext = NULL;
    command_pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    command_pool_info.queueFamilyIndex = queue->vk_family_index;

    if ((vr = VK_CALL(vkCreateCommandPool(device->vk_device, &command_pool_info, NULL,
            &allocator->vk_command_pool))) < 0)
    {
        WARN("Failed to create Vulkan command pool, vr %d.\n", vr);
        vkd3d_private_store_destroy(&allocator->private_store);
        return hresult_from_vk_result(vr);
    }

    allocator->vk_descriptor_pool = VK_NULL_HANDLE;

    allocator->free_descriptor_pools = NULL;
    allocator->free_descriptor_pools_size = 0;
    allocator->free_descriptor_pool_count = 0;

    allocator->passes = NULL;
    allocator->passes_size = 0;
    allocator->pass_count = 0;

    allocator->framebuffers = NULL;
    allocator->framebuffers_size = 0;
    allocator->framebuffer_count = 0;

    allocator->descriptor_pools = NULL;
    allocator->descriptor_pools_size = 0;
    allocator->descriptor_pool_count = 0;

    allocator->views = NULL;
    allocator->views_size = 0;
    allocator->view_count = 0;

    allocator->buffer_views = NULL;
    allocator->buffer_views_size = 0;
    allocator->buffer_view_count = 0;

    allocator->transfer_buffers = NULL;
    allocator->transfer_buffers_size = 0;
    allocator->transfer_buffer_count = 0;

    allocator->command_buffers = NULL;
    allocator->command_buffers_size = 0;
    allocator->command_buffer_count = 0;

    allocator->current_command_list = NULL;

    allocator->device = device;
    ID3D12Device_AddRef(&device->ID3D12Device_iface);

    return S_OK;
}

HRESULT d3d12_command_allocator_create(struct d3d12_device *device,
        D3D12_COMMAND_LIST_TYPE type, struct d3d12_command_allocator **allocator)
{
    struct d3d12_command_allocator *object;
    HRESULT hr;

    if (!(D3D12_COMMAND_LIST_TYPE_DIRECT <= type && type <= D3D12_COMMAND_LIST_TYPE_COPY))
    {
        WARN("Invalid type %#x.\n", type);
        return E_INVALIDARG;
    }

    if (!(object = vkd3d_malloc(sizeof(*object))))
        return E_OUTOFMEMORY;

    if (FAILED(hr = d3d12_command_allocator_init(object, device, type)))
    {
        vkd3d_free(object);
        return hr;
    }

    TRACE("Created command allocator %p.\n", object);

    *allocator = object;

    return S_OK;
}

/* ID3D12CommandList */
static inline struct d3d12_command_list *impl_from_ID3D12GraphicsCommandList(ID3D12GraphicsCommandList *iface)
{
    return CONTAINING_RECORD(iface, struct d3d12_command_list, ID3D12GraphicsCommandList_iface);
}

static void d3d12_command_list_invalidate_current_framebuffer(struct d3d12_command_list *list)
{
    list->current_framebuffer = VK_NULL_HANDLE;
}

static void d3d12_command_list_invalidate_current_pipeline(struct d3d12_command_list *list)
{
    list->current_pipeline = VK_NULL_HANDLE;
}

static void d3d12_command_list_end_current_render_pass(struct d3d12_command_list *list)
{
    const struct vkd3d_vk_device_procs *vk_procs = &list->device->vk_procs;

    if (list->xfb_enabled)
    {
        VK_CALL(vkCmdEndTransformFeedbackEXT(list->vk_command_buffer, 0, ARRAY_SIZE(list->so_counter_buffers),
                list->so_counter_buffers, list->so_counter_buffer_offsets));
    }

    if (list->current_render_pass)
        VK_CALL(vkCmdEndRenderPass(list->vk_command_buffer));

    list->current_render_pass = VK_NULL_HANDLE;

    if (list->xfb_enabled)
    {
        VkMemoryBarrier vk_barrier;

        /* We need a barrier between pause and resume. */
        vk_barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        vk_barrier.pNext = NULL;
        vk_barrier.srcAccessMask = VK_ACCESS_TRANSFORM_FEEDBACK_COUNTER_WRITE_BIT_EXT;
        vk_barrier.dstAccessMask = VK_ACCESS_TRANSFORM_FEEDBACK_COUNTER_READ_BIT_EXT;
        VK_CALL(vkCmdPipelineBarrier(list->vk_command_buffer,
                VK_PIPELINE_STAGE_TRANSFORM_FEEDBACK_BIT_EXT, VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT, 0,
                1, &vk_barrier, 0, NULL, 0, NULL));

        list->xfb_enabled = false;
    }
}

static void d3d12_command_list_invalidate_current_render_pass(struct d3d12_command_list *list)
{
    d3d12_command_list_end_current_render_pass(list);
}

static void d3d12_command_list_invalidate_bindings(struct d3d12_command_list *list,
        struct d3d12_pipeline_state *state)
{
    if (!state)
        return;

    if (state->uav_counter_mask)
    {
        struct vkd3d_pipeline_bindings *bindings = &list->pipeline_bindings[state->vk_bind_point];
        bindings->uav_counter_dirty_mask = ~(uint8_t)0;
    }
}

static bool vk_barrier_parameters_from_d3d12_resource_state(unsigned int state,
        bool is_swapchain_image, D3D12_RESOURCE_STATES present_state, VkQueueFlags vk_queue_flags,
        VkAccessFlags *access_mask, VkPipelineStageFlags *stage_flags, VkImageLayout *image_layout)
{
    VkPipelineStageFlags queue_shader_stages = 0;

    if (vk_queue_flags & VK_QUEUE_GRAPHICS_BIT)
    {
        queue_shader_stages |= VK_PIPELINE_STAGE_VERTEX_SHADER_BIT
                | VK_PIPELINE_STAGE_TESSELLATION_CONTROL_SHADER_BIT
                | VK_PIPELINE_STAGE_TESSELLATION_EVALUATION_SHADER_BIT
                | VK_PIPELINE_STAGE_GEOMETRY_SHADER_BIT
                | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    }
    if (vk_queue_flags & VK_QUEUE_COMPUTE_BIT)
        queue_shader_stages |= VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;

    switch (state)
    {
        case D3D12_RESOURCE_STATE_COMMON: /* D3D12_RESOURCE_STATE_PRESENT */
            /* The COMMON state is used for ownership transfer between
             * DIRECT/COMPUTE and COPY queues. Additionally, a texture has to
             * be in the COMMON state to be accessed by CPU. Moreover,
             * resources can be implicitly promoted to other states out of the
             * COMMON state, and the resource state can decay to the COMMON
             * state when GPU finishes execution of a command list. */
            if (is_swapchain_image)
            {
                if (present_state == D3D12_RESOURCE_STATE_PRESENT)
                {
                    *access_mask = VK_ACCESS_MEMORY_READ_BIT;
                    *stage_flags = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
                    if (image_layout)
                        *image_layout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
                }
                else
                {
                    vk_barrier_parameters_from_d3d12_resource_state(present_state,
                            false, 0, vk_queue_flags, access_mask, stage_flags, image_layout);
                }
            }
            else
            {
                *access_mask = VK_ACCESS_HOST_READ_BIT | VK_ACCESS_HOST_WRITE_BIT;
                *stage_flags = VK_PIPELINE_STAGE_HOST_BIT;
                if (image_layout)
                    *image_layout = VK_IMAGE_LAYOUT_GENERAL;
            }
            return true;

        /* Handle write states. */
        case D3D12_RESOURCE_STATE_RENDER_TARGET:
            *access_mask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT
                    | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            *stage_flags = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
            if (image_layout)
                *image_layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            return true;

        case D3D12_RESOURCE_STATE_UNORDERED_ACCESS:
            *access_mask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
            *stage_flags = queue_shader_stages;
            if (image_layout)
                *image_layout = VK_IMAGE_LAYOUT_GENERAL;
            return true;

        case D3D12_RESOURCE_STATE_DEPTH_WRITE:
            *access_mask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT
                    | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
            *stage_flags = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT
                    | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
            if (image_layout)
                *image_layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            return true;

        case D3D12_RESOURCE_STATE_COPY_DEST:
        case D3D12_RESOURCE_STATE_RESOLVE_DEST:
            *access_mask = VK_ACCESS_TRANSFER_WRITE_BIT;
            *stage_flags = VK_PIPELINE_STAGE_TRANSFER_BIT;
            if (image_layout)
                *image_layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            return true;

        case D3D12_RESOURCE_STATE_STREAM_OUT:
            *access_mask = VK_ACCESS_TRANSFORM_FEEDBACK_WRITE_BIT_EXT
                    | VK_ACCESS_TRANSFORM_FEEDBACK_COUNTER_READ_BIT_EXT
                    | VK_ACCESS_TRANSFORM_FEEDBACK_COUNTER_WRITE_BIT_EXT;
            *stage_flags = VK_PIPELINE_STAGE_TRANSFORM_FEEDBACK_BIT_EXT
                    | VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT;
            if (image_layout)
                *image_layout = VK_IMAGE_LAYOUT_UNDEFINED;
            return true;

        /* Set the Vulkan image layout for read-only states. */
        case D3D12_RESOURCE_STATE_DEPTH_READ:
        case D3D12_RESOURCE_STATE_DEPTH_READ | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE:
        case D3D12_RESOURCE_STATE_DEPTH_READ | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE:
        case D3D12_RESOURCE_STATE_DEPTH_READ | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE
                | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE:
            *access_mask = 0;
            *stage_flags = 0;
            if (image_layout)
                *image_layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
            break;

        case D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE:
        case D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE:
        case D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE:
            *access_mask = 0;
            *stage_flags = 0;
            if (image_layout)
                *image_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            break;

        case D3D12_RESOURCE_STATE_COPY_SOURCE:
        case D3D12_RESOURCE_STATE_RESOLVE_SOURCE:
            *access_mask = 0;
            *stage_flags = 0;
            if (image_layout)
                *image_layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            break;

        default:
            *access_mask = 0;
            *stage_flags = 0;
            if (image_layout)
                *image_layout = VK_IMAGE_LAYOUT_GENERAL;
            break;
    }

    /* Handle read-only states. */
    assert(!is_write_resource_state(state));

    if (state & D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER)
    {
        *access_mask |= VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT
                | VK_ACCESS_UNIFORM_READ_BIT;
        *stage_flags |= VK_PIPELINE_STAGE_VERTEX_INPUT_BIT
                | queue_shader_stages;
        state &= ~D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
    }

    if (state & D3D12_RESOURCE_STATE_INDEX_BUFFER)
    {
        *access_mask |= VK_ACCESS_INDEX_READ_BIT;
        *stage_flags |= VK_PIPELINE_STAGE_VERTEX_INPUT_BIT;
        state &= ~D3D12_RESOURCE_STATE_INDEX_BUFFER;
    }

    if (state & D3D12_RESOURCE_STATE_DEPTH_READ)
    {
        *access_mask |= VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
        *stage_flags |= VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT
                | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        state &= ~D3D12_RESOURCE_STATE_DEPTH_READ;
    }

    if (state & D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)
    {
        *access_mask |= VK_ACCESS_SHADER_READ_BIT;
        *stage_flags |= (queue_shader_stages & ~VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
        state &= ~D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    }
    if (state & D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)
    {
        *access_mask |= VK_ACCESS_SHADER_READ_BIT;
        *stage_flags |= VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        state &= ~D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    }

    if (state & D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT) /* D3D12_RESOURCE_STATE_PREDICATION */
    {
        *access_mask |= VK_ACCESS_INDIRECT_COMMAND_READ_BIT;
        *stage_flags |= VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT;
        state &= ~D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT;
    }

    if (state & (D3D12_RESOURCE_STATE_COPY_SOURCE | D3D12_RESOURCE_STATE_RESOLVE_SOURCE))
    {
        *access_mask |= VK_ACCESS_TRANSFER_READ_BIT;
        *stage_flags |= VK_PIPELINE_STAGE_TRANSFER_BIT;
        state &= ~(D3D12_RESOURCE_STATE_COPY_SOURCE | D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
    }

    if (state)
    {
        WARN("Invalid resource state %#x.\n", state);
        return false;
    }
    return true;
}

static void d3d12_command_list_transition_resource_to_initial_state(struct d3d12_command_list *list,
        struct d3d12_resource *resource)
{
    const struct vkd3d_vk_device_procs *vk_procs = &list->device->vk_procs;
    VkPipelineStageFlags src_stage_mask, dst_stage_mask;
    const struct vkd3d_format *format;
    VkImageMemoryBarrier barrier;

    assert(d3d12_resource_is_texture(resource));

    if (!(format = vkd3d_format_from_d3d12_resource_desc(&resource->desc, 0)))
    {
        ERR("Resource %p has invalid format %#x.\n", resource, resource->desc.Format);
        return;
    }

    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.pNext = NULL;

    /* vkQueueSubmit() defines a memory dependency with prior host writes. */
    src_stage_mask = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    barrier.srcAccessMask = 0;
    barrier.oldLayout = is_cpu_accessible_heap(&resource->heap_properties) ?
            VK_IMAGE_LAYOUT_PREINITIALIZED : VK_IMAGE_LAYOUT_UNDEFINED;

    if (!vk_barrier_parameters_from_d3d12_resource_state(resource->initial_state,
            resource->flags & VKD3D_RESOURCE_PRESENT_STATE_TRANSITION, resource->present_state,
            list->vk_queue_flags, &barrier.dstAccessMask, &dst_stage_mask, &barrier.newLayout))
    {
        FIXME("Unhandled state %#x.\n", resource->initial_state);
        return;
    }

    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = resource->u.vk_image;
    barrier.subresourceRange.aspectMask = format->vk_aspect_mask;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = VK_REMAINING_MIP_LEVELS;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = VK_REMAINING_ARRAY_LAYERS;

    TRACE("Initial state %#x transition for resource %p (old layout %#x, new layout %#x).\n",
            resource->initial_state, resource, barrier.oldLayout, barrier.newLayout);

    VK_CALL(vkCmdPipelineBarrier(list->vk_command_buffer, src_stage_mask, dst_stage_mask, 0,
            0, NULL, 0, NULL, 1, &barrier));
}

static void d3d12_command_list_track_resource_usage(struct d3d12_command_list *list,
        struct d3d12_resource *resource)
{
    if (resource->flags & VKD3D_RESOURCE_INITIAL_STATE_TRANSITION)
    {
        d3d12_command_list_end_current_render_pass(list);

        d3d12_command_list_transition_resource_to_initial_state(list, resource);
        resource->flags &= ~VKD3D_RESOURCE_INITIAL_STATE_TRANSITION;
    }
}

static HRESULT STDMETHODCALLTYPE d3d12_command_list_QueryInterface(ID3D12GraphicsCommandList *iface,
        REFIID riid, void **object)
{
    TRACE("iface %p, riid %s, object %p.\n", iface, debugstr_guid(riid), object);

    if (IsEqualGUID(riid, &IID_ID3D12GraphicsCommandList)
            || IsEqualGUID(riid, &IID_ID3D12CommandList)
            || IsEqualGUID(riid, &IID_ID3D12DeviceChild)
            || IsEqualGUID(riid, &IID_ID3D12Object)
            || IsEqualGUID(riid, &IID_IUnknown))
    {
        ID3D12GraphicsCommandList_AddRef(iface);
        *object = iface;
        return S_OK;
    }

    WARN("%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid(riid));

    *object = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE d3d12_command_list_AddRef(ID3D12GraphicsCommandList *iface)
{
    struct d3d12_command_list *list = impl_from_ID3D12GraphicsCommandList(iface);
    ULONG refcount = InterlockedIncrement(&list->refcount);

    TRACE("%p increasing refcount to %u.\n", list, refcount);

    return refcount;
}

static ULONG STDMETHODCALLTYPE d3d12_command_list_Release(ID3D12GraphicsCommandList *iface)
{
    struct d3d12_command_list *list = impl_from_ID3D12GraphicsCommandList(iface);
    ULONG refcount = InterlockedDecrement(&list->refcount);

    TRACE("%p decreasing refcount to %u.\n", list, refcount);

    if (!refcount)
    {
        struct d3d12_device *device = list->device;

        vkd3d_private_store_destroy(&list->private_store);

        /* When command pool is destroyed, all command buffers are implicitly freed. */
        if (list->allocator)
            d3d12_command_allocator_free_command_buffer(list->allocator, list);

        vkd3d_free(list);

        ID3D12Device_Release(&device->ID3D12Device_iface);
    }

    return refcount;
}

static HRESULT STDMETHODCALLTYPE d3d12_command_list_GetPrivateData(ID3D12GraphicsCommandList *iface,
        REFGUID guid, UINT *data_size, void *data)
{
    struct d3d12_command_list *list = impl_from_ID3D12GraphicsCommandList(iface);

    TRACE("iface %p, guid %s, data_size %p, data %p.\n", iface, debugstr_guid(guid), data_size, data);

    return vkd3d_get_private_data(&list->private_store, guid, data_size, data);
}

static HRESULT STDMETHODCALLTYPE d3d12_command_list_SetPrivateData(ID3D12GraphicsCommandList *iface,
        REFGUID guid, UINT data_size, const void *data)
{
    struct d3d12_command_list *list = impl_from_ID3D12GraphicsCommandList(iface);

    TRACE("iface %p, guid %s, data_size %u, data %p.\n", iface, debugstr_guid(guid), data_size, data);

    return vkd3d_set_private_data(&list->private_store, guid, data_size, data);
}

static HRESULT STDMETHODCALLTYPE d3d12_command_list_SetPrivateDataInterface(ID3D12GraphicsCommandList *iface,
        REFGUID guid, const IUnknown *data)
{
    struct d3d12_command_list *list = impl_from_ID3D12GraphicsCommandList(iface);

    TRACE("iface %p, guid %s, data %p.\n", iface, debugstr_guid(guid), data);

    return vkd3d_set_private_data_interface(&list->private_store, guid, data);
}

static HRESULT STDMETHODCALLTYPE d3d12_command_list_SetName(ID3D12GraphicsCommandList *iface, const WCHAR *name)
{
    struct d3d12_command_list *list = impl_from_ID3D12GraphicsCommandList(iface);

    FIXME("iface %p, name %s stub!\n", iface, debugstr_w(name, list->device->wchar_size));

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE d3d12_command_list_GetDevice(ID3D12GraphicsCommandList *iface,
        REFIID riid, void **device)
{
    struct d3d12_command_list *list = impl_from_ID3D12GraphicsCommandList(iface);

    TRACE("iface %p, riid %s, device %p.\n", iface, debugstr_guid(riid), device);

    return ID3D12Device_QueryInterface(&list->device->ID3D12Device_iface, riid, device);
}

static D3D12_COMMAND_LIST_TYPE STDMETHODCALLTYPE d3d12_command_list_GetType(ID3D12GraphicsCommandList *iface)
{
    struct d3d12_command_list *list = impl_from_ID3D12GraphicsCommandList(iface);

    TRACE("iface %p.\n", iface);

    return list->type;
}

static HRESULT STDMETHODCALLTYPE d3d12_command_list_Close(ID3D12GraphicsCommandList *iface)
{
    struct d3d12_command_list *list = impl_from_ID3D12GraphicsCommandList(iface);
    const struct vkd3d_vk_device_procs *vk_procs;
    VkResult vr;

    TRACE("iface %p.\n", iface);

    if (!list->is_recording)
    {
        WARN("Command list is not in the recording state.\n");
        return E_FAIL;
    }

    vk_procs = &list->device->vk_procs;

    d3d12_command_list_end_current_render_pass(list);

    if ((vr = VK_CALL(vkEndCommandBuffer(list->vk_command_buffer))) < 0)
    {
        WARN("Failed to end command buffer, vr %d.\n", vr);
        return hresult_from_vk_result(vr);
    }

    if (list->allocator)
    {
        d3d12_command_allocator_free_command_buffer(list->allocator, list);
        list->allocator = NULL;
    }

    list->is_recording = false;

    if (!list->is_valid)
    {
        WARN("Error occurred during command list recording.\n");
        return E_INVALIDARG;
    }

    return S_OK;
}

static void d3d12_command_list_reset_state(struct d3d12_command_list *list,
        ID3D12PipelineState *initial_pipeline_state)
{
    ID3D12GraphicsCommandList *iface = &list->ID3D12GraphicsCommandList_iface;

    memset(list->strides, 0, sizeof(list->strides));
    list->primitive_topology = VK_PRIMITIVE_TOPOLOGY_POINT_LIST;

    list->index_buffer_format = DXGI_FORMAT_UNKNOWN;

    memset(list->views, 0, sizeof(list->views));
    list->fb_width = 0;
    list->fb_height = 0;
    list->fb_layer_count = 0;

    list->xfb_enabled = false;

    list->current_framebuffer = VK_NULL_HANDLE;
    list->current_pipeline = VK_NULL_HANDLE;
    list->current_render_pass = VK_NULL_HANDLE;

    memset(list->pipeline_bindings, 0, sizeof(list->pipeline_bindings));

    list->state = NULL;

    memset(list->so_counter_buffers, 0, sizeof(list->so_counter_buffers));
    memset(list->so_counter_buffer_offsets, 0, sizeof(list->so_counter_buffer_offsets));

    ID3D12GraphicsCommandList_SetPipelineState(iface, initial_pipeline_state);
}

static HRESULT STDMETHODCALLTYPE d3d12_command_list_Reset(ID3D12GraphicsCommandList *iface,
        ID3D12CommandAllocator *allocator, ID3D12PipelineState *initial_pipeline_state)
{
    struct d3d12_command_allocator *allocator_impl = unsafe_impl_from_ID3D12CommandAllocator(allocator);
    struct d3d12_command_list *list = impl_from_ID3D12GraphicsCommandList(iface);
    HRESULT hr;

    TRACE("iface %p, allocator %p, initial_pipeline_state %p.\n",
            iface, allocator, initial_pipeline_state);

    if (!allocator_impl)
    {
        WARN("Command allocator is NULL.\n");
        return E_INVALIDARG;
    }

    if (list->is_recording)
    {
        WARN("Command list is in the recording state.\n");
        return E_FAIL;
    }

    if (SUCCEEDED(hr = d3d12_command_allocator_allocate_command_buffer(allocator_impl, list)))
    {
        list->allocator = allocator_impl;
        d3d12_command_list_reset_state(list, initial_pipeline_state);
    }

    return hr;
}

static HRESULT STDMETHODCALLTYPE d3d12_command_list_ClearState(ID3D12GraphicsCommandList *iface,
        ID3D12PipelineState *pipeline_state)
{
    FIXME("iface %p, pipline_state %p stub!\n", iface, pipeline_state);

    return E_NOTIMPL;
}

static void d3d12_command_list_get_fb_extent(struct d3d12_command_list *list,
        uint32_t *width, uint32_t *height, uint32_t *layer_count)
{
    struct d3d12_device *device = list->device;

    if (list->state->u.graphics.attachment_count)
    {
        *width = list->fb_width;
        *height = list->fb_height;
        if (layer_count)
            *layer_count = list->fb_layer_count;
    }
    else
    {
        *width = device->vk_info.device_limits.maxFramebufferWidth;
        *height = device->vk_info.device_limits.maxFramebufferHeight;
        if (layer_count)
            *layer_count = 1;
    }
}

static bool d3d12_command_list_update_current_framebuffer(struct d3d12_command_list *list)
{
    struct d3d12_device *device = list->device;
    const struct vkd3d_vk_device_procs *vk_procs = &device->vk_procs;
    struct VkFramebufferCreateInfo fb_desc;
    VkFramebuffer vk_framebuffer;
    size_t start_idx = 0;
    VkResult vr;

    if (list->current_framebuffer != VK_NULL_HANDLE)
        return true;

    if (!list->state->u.graphics.rt_idx)
        ++start_idx;

    fb_desc.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fb_desc.pNext = NULL;
    fb_desc.flags = 0;
    fb_desc.renderPass = list->state->u.graphics.render_pass;
    fb_desc.attachmentCount = list->state->u.graphics.attachment_count;
    fb_desc.pAttachments = &list->views[start_idx];
    d3d12_command_list_get_fb_extent(list, &fb_desc.width, &fb_desc.height, &fb_desc.layers);
    if ((vr = VK_CALL(vkCreateFramebuffer(device->vk_device, &fb_desc, NULL, &vk_framebuffer))) < 0)
    {
        WARN("Failed to create Vulkan framebuffer, vr %d.\n", vr);
        return false;
    }

    if (!d3d12_command_allocator_add_framebuffer(list->allocator, vk_framebuffer))
    {
        WARN("Failed to add framebuffer.\n");
        VK_CALL(vkDestroyFramebuffer(device->vk_device, vk_framebuffer, NULL));
        return false;
    }

    list->current_framebuffer = vk_framebuffer;

    return true;
}

static bool d3d12_command_list_update_current_pipeline(struct d3d12_command_list *list)
{
    const struct vkd3d_vk_device_procs *vk_procs = &list->device->vk_procs;
    VkPipeline vk_pipeline;

    if (list->current_pipeline != VK_NULL_HANDLE)
        return true;

    if (!d3d12_pipeline_state_is_graphics(list->state))
    {
        WARN("Pipeline state %p is not a graphics pipeline.\n", list->state);
        return false;
    }

    if (!(vk_pipeline = d3d12_pipeline_state_get_or_create_pipeline(list->state,
            list->primitive_topology, list->strides)))
        return false;

    VK_CALL(vkCmdBindPipeline(list->vk_command_buffer, list->state->vk_bind_point, vk_pipeline));
    list->current_pipeline = vk_pipeline;

    return true;
}

static void d3d12_command_list_prepare_descriptors(struct d3d12_command_list *list,
        VkPipelineBindPoint bind_point)
{
    struct vkd3d_pipeline_bindings *bindings = &list->pipeline_bindings[bind_point];
    const struct d3d12_root_signature *root_signature = bindings->root_signature;

    if (bindings->descriptor_set && !bindings->in_use)
        return;

    /* We cannot modify bound descriptor sets. We need a new descriptor set if
     * we are about to update resource bindings.
     *
     * The Vulkan spec says:
     *
     *   "The descriptor set contents bound by a call to
     *   vkCmdBindDescriptorSets may be consumed during host execution of the
     *   command, or during shader execution of the resulting draws, or any
     *   time in between. Thus, the contents must not be altered (overwritten
     *   by an update command, or freed) between when the command is recorded
     *   and when the command completes executing on the queue."
     */
    bindings->descriptor_set = d3d12_command_allocator_allocate_descriptor_set(list->allocator,
            root_signature->vk_set_layout);
    bindings->in_use = false;

    bindings->descriptor_table_dirty_mask |= bindings->descriptor_table_active_mask;
    bindings->push_descriptor_dirty_mask |= bindings->push_descriptor_active_mask;
}

static bool vk_write_descriptor_set_from_d3d12_desc(VkWriteDescriptorSet *vk_descriptor_write,
        VkDescriptorImageInfo *vk_image_info, const struct d3d12_desc *descriptor,
        uint32_t descriptor_range_magic, VkDescriptorSet vk_descriptor_set,
        uint32_t vk_binding, unsigned int index)
{
    const struct vkd3d_view *view = descriptor->u.view;

    if (descriptor->magic != descriptor_range_magic)
        return false;

    vk_descriptor_write->sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    vk_descriptor_write->pNext = NULL;
    vk_descriptor_write->dstSet = vk_descriptor_set;
    vk_descriptor_write->dstBinding = vk_binding + index;
    vk_descriptor_write->dstArrayElement = 0;
    vk_descriptor_write->descriptorCount = 1;
    vk_descriptor_write->descriptorType = descriptor->vk_descriptor_type;
    vk_descriptor_write->pImageInfo = NULL;
    vk_descriptor_write->pBufferInfo = NULL;
    vk_descriptor_write->pTexelBufferView = NULL;

    switch (descriptor->magic)
    {
        case VKD3D_DESCRIPTOR_MAGIC_CBV:
            vk_descriptor_write->pBufferInfo = &descriptor->u.vk_cbv_info;
            break;

        case VKD3D_DESCRIPTOR_MAGIC_SRV:
        case VKD3D_DESCRIPTOR_MAGIC_UAV:
            /* We use separate bindings for buffer and texture SRVs/UAVs.
             * See d3d12_root_signature_init(). */
            vk_descriptor_write->dstBinding = vk_binding + 2 * index;
            if (descriptor->vk_descriptor_type != VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER
                    && descriptor->vk_descriptor_type != VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER)
                ++vk_descriptor_write->dstBinding;

            if (descriptor->vk_descriptor_type == VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER
                    || descriptor->vk_descriptor_type == VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER)
            {
                vk_descriptor_write->pTexelBufferView = &view->u.vk_buffer_view;
            }
            else
            {
                vk_image_info->sampler = VK_NULL_HANDLE;
                vk_image_info->imageView = view->u.vk_image_view;
                vk_image_info->imageLayout = descriptor->magic == VKD3D_DESCRIPTOR_MAGIC_SRV
                        ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_GENERAL;

                vk_descriptor_write->pImageInfo = vk_image_info;
            }
            break;

        case VKD3D_DESCRIPTOR_MAGIC_SAMPLER:
            vk_image_info->sampler = view->u.vk_sampler;
            vk_image_info->imageView = VK_NULL_HANDLE;
            vk_image_info->imageLayout = VK_IMAGE_LAYOUT_UNDEFINED;

            vk_descriptor_write->pImageInfo = vk_image_info;
            break;

        default:
            ERR("Invalid descriptor %#x.\n", descriptor->magic);
            return false;
    }

    return true;
}

static void d3d12_command_list_update_descriptor_table(struct d3d12_command_list *list,
        VkPipelineBindPoint bind_point, unsigned int index, D3D12_GPU_DESCRIPTOR_HANDLE base_descriptor)
{
    struct vkd3d_pipeline_bindings *bindings = &list->pipeline_bindings[bind_point];
    struct VkWriteDescriptorSet descriptor_writes[24], *current_descriptor_write;
    const struct d3d12_root_signature *root_signature = bindings->root_signature;
    const struct vkd3d_vk_device_procs *vk_procs = &list->device->vk_procs;
    struct VkDescriptorImageInfo image_infos[24], *current_image_info;
    const struct d3d12_root_descriptor_table *descriptor_table;
    const struct d3d12_root_descriptor_table_range *range;
    VkDevice vk_device = list->device->vk_device;
    unsigned int i, j, descriptor_count;
    struct d3d12_desc *descriptor;

    assert(root_signature->parameters[index].parameter_type == D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE);
    descriptor_table = &root_signature->parameters[index].u.descriptor_table;

    descriptor_count = 0;
    for (i = 0; i < descriptor_table->range_count; ++i)
        descriptor_count += descriptor_table->ranges[i].descriptor_count;

    if (!descriptor_count)
        return;

    if (!(descriptor = d3d12_desc_from_gpu_handle(base_descriptor)))
    {
        WARN("Descriptor table %u is not set.\n", index);
        return;
    }

    descriptor_count = 0;
    current_descriptor_write = descriptor_writes;
    current_image_info = image_infos;
    for (i = 0; i < descriptor_table->range_count; ++i)
    {
        range = &descriptor_table->ranges[i];

        if (range->offset != D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND)
        {
            descriptor = d3d12_desc_from_gpu_handle(base_descriptor) + range->offset;
        }

        for (j = 0; j < range->descriptor_count; ++j, ++descriptor)
        {
            unsigned int register_idx = range->base_register_idx + j;

            /* Track UAV counters. */
            if (range->descriptor_magic == VKD3D_DESCRIPTOR_MAGIC_UAV
                    && register_idx < ARRAY_SIZE(bindings->vk_uav_counter_views))
            {
                VkBufferView vk_counter_view = descriptor->magic == VKD3D_DESCRIPTOR_MAGIC_UAV
                        ? descriptor->u.view->vk_counter_view : VK_NULL_HANDLE;
                if (bindings->vk_uav_counter_views[register_idx] != vk_counter_view)
                    bindings->uav_counter_dirty_mask |= 1u << register_idx;
                bindings->vk_uav_counter_views[register_idx] = vk_counter_view;
            }

            if (!vk_write_descriptor_set_from_d3d12_desc(current_descriptor_write,
                    current_image_info, descriptor, range->descriptor_magic,
                    bindings->descriptor_set, range->binding, j))
                continue;

            ++descriptor_count;
            ++current_descriptor_write;
            ++current_image_info;

            if (descriptor_count == ARRAY_SIZE(descriptor_writes))
            {
                VK_CALL(vkUpdateDescriptorSets(vk_device, descriptor_count, descriptor_writes, 0, NULL));
                descriptor_count = 0;
                current_descriptor_write = descriptor_writes;
                current_image_info = image_infos;
            }
        }
    }

    VK_CALL(vkUpdateDescriptorSets(vk_device, descriptor_count, descriptor_writes, 0, NULL));
}

static bool vk_write_descriptor_set_from_root_descriptor(VkWriteDescriptorSet *vk_descriptor_write,
        const struct d3d12_root_parameter *root_parameter, VkDescriptorSet vk_descriptor_set,
        VkBufferView *vk_buffer_view, const VkDescriptorBufferInfo *vk_buffer_info)
{
    const struct d3d12_root_descriptor *root_descriptor;

    switch (root_parameter->parameter_type)
    {
        case D3D12_ROOT_PARAMETER_TYPE_CBV:
            vk_descriptor_write->descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            break;
        case D3D12_ROOT_PARAMETER_TYPE_SRV:
            vk_descriptor_write->descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
            break;
        case D3D12_ROOT_PARAMETER_TYPE_UAV:
            vk_descriptor_write->descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER;
            break;
        default:
            ERR("Invalid root descriptor.\n");
            return false;
    }

    root_descriptor = &root_parameter->u.descriptor;

    vk_descriptor_write->sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    vk_descriptor_write->pNext = NULL;
    vk_descriptor_write->dstSet = vk_descriptor_set;
    vk_descriptor_write->dstBinding = root_descriptor->binding;
    vk_descriptor_write->dstArrayElement = 0;
    vk_descriptor_write->descriptorCount = 1;
    vk_descriptor_write->pImageInfo = NULL;
    vk_descriptor_write->pBufferInfo = vk_buffer_info;
    vk_descriptor_write->pTexelBufferView = vk_buffer_view;

    return true;
}

static void d3d12_command_list_update_push_descriptors(struct d3d12_command_list *list,
        VkPipelineBindPoint bind_point)
{
    struct vkd3d_pipeline_bindings *bindings = &list->pipeline_bindings[bind_point];
    const struct d3d12_root_signature *root_signature = bindings->root_signature;
    VkWriteDescriptorSet *descriptor_writes = NULL, *current_descriptor_write;
    const struct vkd3d_vk_device_procs *vk_procs = &list->device->vk_procs;
    VkDescriptorBufferInfo *buffer_infos = NULL, *current_buffer_info;
    const struct d3d12_root_parameter *root_parameter;
    struct vkd3d_push_descriptor *push_descriptor;
    struct d3d12_device *device = list->device;
    VkDescriptorBufferInfo *vk_buffer_info;
    unsigned int i, descriptor_count;
    VkBufferView *vk_buffer_view;

    if (!bindings->push_descriptor_dirty_mask)
        return;

    descriptor_count = vkd3d_popcount(bindings->push_descriptor_dirty_mask);

    if (!(descriptor_writes = vkd3d_calloc(descriptor_count, sizeof(*descriptor_writes))))
        return;
    if (!(buffer_infos = vkd3d_calloc(descriptor_count, sizeof(*buffer_infos))))
        goto done;

    descriptor_count = 0;
    current_buffer_info = buffer_infos;
    current_descriptor_write = descriptor_writes;
    for (i = 0; i < ARRAY_SIZE(bindings->push_descriptors); ++i)
    {
        if (!(bindings->push_descriptor_dirty_mask & (1u << i)))
            continue;

        root_parameter = &root_signature->parameters[i];
        push_descriptor = &bindings->push_descriptors[i];

        if (root_parameter->parameter_type == D3D12_ROOT_PARAMETER_TYPE_CBV)
        {
            vk_buffer_view = NULL;
            vk_buffer_info = current_buffer_info;
            vk_buffer_info->buffer = push_descriptor->u.cbv.vk_buffer;
            vk_buffer_info->offset = push_descriptor->u.cbv.offset;
            vk_buffer_info->range = VK_WHOLE_SIZE;
        }
        else
        {
            vk_buffer_view = &push_descriptor->u.vk_buffer_view;
            vk_buffer_info = NULL;
        }

        if (!vk_write_descriptor_set_from_root_descriptor(current_descriptor_write,
                root_parameter, bindings->descriptor_set, vk_buffer_view, vk_buffer_info))
            continue;

        ++descriptor_count;
        ++current_descriptor_write;
        ++current_buffer_info;
    }

    VK_CALL(vkUpdateDescriptorSets(device->vk_device, descriptor_count, descriptor_writes, 0, NULL));
    bindings->push_descriptor_dirty_mask = 0;

done:
    vkd3d_free(descriptor_writes);
    vkd3d_free(buffer_infos);
}

static void d3d12_command_list_update_uav_counter_descriptors(struct d3d12_command_list *list,
        VkPipelineBindPoint bind_point)
{
    struct vkd3d_pipeline_bindings *bindings = &list->pipeline_bindings[bind_point];
    const struct vkd3d_vk_device_procs *vk_procs = &list->device->vk_procs;
    const struct d3d12_pipeline_state *state = list->state;
    VkDevice vk_device = list->device->vk_device;
    VkWriteDescriptorSet *vk_descriptor_writes;
    VkDescriptorSet vk_descriptor_set;
    unsigned int uav_counter_count;
    unsigned int i;

    if (!state || !(state->uav_counter_mask & bindings->uav_counter_dirty_mask))
        return;

    uav_counter_count = vkd3d_popcount(state->uav_counter_mask);

    vk_descriptor_set = d3d12_command_allocator_allocate_descriptor_set(list->allocator, state->vk_set_layout);
    if (!vk_descriptor_set)
        return;

    if (!(vk_descriptor_writes = vkd3d_calloc(uav_counter_count, sizeof(*vk_descriptor_writes))))
        return;

    for (i = 0; i < uav_counter_count; ++i)
    {
        const struct vkd3d_shader_uav_counter_binding *uav_counter = &state->uav_counters[i];
        const VkBufferView *vk_uav_counter_views = bindings->vk_uav_counter_views;

        assert(vk_uav_counter_views[uav_counter->register_index]);

        vk_descriptor_writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        vk_descriptor_writes[i].pNext = NULL;
        vk_descriptor_writes[i].dstSet = vk_descriptor_set;
        vk_descriptor_writes[i].dstBinding = uav_counter->binding.binding;
        vk_descriptor_writes[i].dstArrayElement = 0;
        vk_descriptor_writes[i].descriptorCount = 1;
        vk_descriptor_writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER;
        vk_descriptor_writes[i].pImageInfo = NULL;
        vk_descriptor_writes[i].pBufferInfo = NULL;
        vk_descriptor_writes[i].pTexelBufferView = &vk_uav_counter_views[uav_counter->register_index];
    }

    VK_CALL(vkUpdateDescriptorSets(vk_device, uav_counter_count, vk_descriptor_writes, 0, NULL));
    vkd3d_free(vk_descriptor_writes);

    VK_CALL(vkCmdBindDescriptorSets(list->vk_command_buffer, bind_point,
            state->vk_pipeline_layout, state->set_index, 1, &vk_descriptor_set, 0, NULL));

    bindings->uav_counter_dirty_mask = 0;
}

static void d3d12_command_list_update_descriptors(struct d3d12_command_list *list,
        VkPipelineBindPoint bind_point)
{
    struct vkd3d_pipeline_bindings *bindings = &list->pipeline_bindings[bind_point];
    const struct vkd3d_vk_device_procs *vk_procs = &list->device->vk_procs;
    const struct d3d12_root_signature *rs = bindings->root_signature;
    unsigned int i;

    if (!rs || !rs->pool_size_count || !rs->vk_set_layout)
        return;

    if (bindings->descriptor_table_dirty_mask || bindings->push_descriptor_dirty_mask)
        d3d12_command_list_prepare_descriptors(list, bind_point);

    for (i = 0; i < ARRAY_SIZE(bindings->descriptor_tables); ++i)
    {
        if (bindings->descriptor_table_dirty_mask & ((uint64_t)1 << i))
            d3d12_command_list_update_descriptor_table(list, bind_point, i, bindings->descriptor_tables[i]);
    }
    bindings->descriptor_table_dirty_mask = 0;

    d3d12_command_list_update_push_descriptors(list, bind_point);

    if (bindings->descriptor_set)
    {
        VK_CALL(vkCmdBindDescriptorSets(list->vk_command_buffer, bind_point,
                rs->vk_pipeline_layout, rs->main_set, 1, &bindings->descriptor_set, 0, NULL));
        bindings->in_use = true;
    }

    d3d12_command_list_update_uav_counter_descriptors(list, bind_point);
}

static bool d3d12_command_list_begin_render_pass(struct d3d12_command_list *list)
{
    const struct vkd3d_vk_device_procs *vk_procs = &list->device->vk_procs;
    struct d3d12_graphics_pipeline_state *graphics;
    struct VkRenderPassBeginInfo begin_desc;
    VkRenderPass vk_render_pass;

    if (!list->state)
    {
        WARN("Pipeline state is NULL.\n");
        return false;
    }

    if (!d3d12_command_list_update_current_framebuffer(list))
        return false;
    if (!d3d12_command_list_update_current_pipeline(list))
        return false;

    d3d12_command_list_update_descriptors(list, VK_PIPELINE_BIND_POINT_GRAPHICS);

    if (list->current_render_pass != VK_NULL_HANDLE)
        return true;

    vk_render_pass = list->state->u.graphics.render_pass;

    begin_desc.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    begin_desc.pNext = NULL;
    begin_desc.renderPass = vk_render_pass;
    begin_desc.framebuffer = list->current_framebuffer;
    begin_desc.renderArea.offset.x = 0;
    begin_desc.renderArea.offset.y = 0;
    d3d12_command_list_get_fb_extent(list,
            &begin_desc.renderArea.extent.width, &begin_desc.renderArea.extent.height, NULL);
    begin_desc.clearValueCount = 0;
    begin_desc.pClearValues = NULL;
    VK_CALL(vkCmdBeginRenderPass(list->vk_command_buffer, &begin_desc, VK_SUBPASS_CONTENTS_INLINE));

    list->current_render_pass = vk_render_pass;

    graphics = &list->state->u.graphics;
    if (graphics->xfb_enabled)
    {
        VK_CALL(vkCmdBeginTransformFeedbackEXT(list->vk_command_buffer, 0, ARRAY_SIZE(list->so_counter_buffers),
                list->so_counter_buffers, list->so_counter_buffer_offsets));

        list->xfb_enabled = true;
    }

    return true;
}

static void d3d12_command_list_check_index_buffer_strip_cut_value(struct d3d12_command_list *list)
{
    struct d3d12_graphics_pipeline_state *graphics = &list->state->u.graphics;

    /* In Vulkan, the strip cut value is derived from the index buffer format. */
    switch (graphics->index_buffer_strip_cut_value)
    {
        case D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFF:
            if (list->index_buffer_format != DXGI_FORMAT_R16_UINT)
            {
                FIXME("Strip cut value 0xffff is not supported with index buffer format %#x.\n",
                        list->index_buffer_format);
            }
            break;

        case D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFFFFFF:
            if (list->index_buffer_format != DXGI_FORMAT_R32_UINT)
            {
                FIXME("Strip cut value 0xffffffff is not supported with index buffer format %#x.\n",
                        list->index_buffer_format);
            }
            break;

        default:
            break;
    }
}

static void STDMETHODCALLTYPE d3d12_command_list_DrawInstanced(ID3D12GraphicsCommandList *iface,
        UINT vertex_count_per_instance, UINT instance_count, UINT start_vertex_location,
        UINT start_instance_location)
{
    struct d3d12_command_list *list = impl_from_ID3D12GraphicsCommandList(iface);
    const struct vkd3d_vk_device_procs *vk_procs;

    TRACE("iface %p, vertex_count_per_instance %u, instance_count %u, "
            "start_vertex_location %u, start_instance_location %u.\n",
            iface, vertex_count_per_instance, instance_count,
            start_vertex_location, start_instance_location);

    vk_procs = &list->device->vk_procs;

    if (!d3d12_command_list_begin_render_pass(list))
    {
        WARN("Failed to begin render pass, ignoring draw call.\n");
        return;
    }

    VK_CALL(vkCmdDraw(list->vk_command_buffer, vertex_count_per_instance,
            instance_count, start_vertex_location, start_instance_location));
}

static void STDMETHODCALLTYPE d3d12_command_list_DrawIndexedInstanced(ID3D12GraphicsCommandList *iface,
        UINT index_count_per_instance, UINT instance_count, UINT start_vertex_location,
        INT base_vertex_location, UINT start_instance_location)
{
    struct d3d12_command_list *list = impl_from_ID3D12GraphicsCommandList(iface);
    const struct vkd3d_vk_device_procs *vk_procs;

    TRACE("iface %p, index_count_per_instance %u, instance_count %u, start_vertex_location %u, "
            "base_vertex_location %d, start_instance_location %u.\n",
            iface, index_count_per_instance, instance_count, start_vertex_location,
            base_vertex_location, start_instance_location);

    if (!d3d12_command_list_begin_render_pass(list))
    {
        WARN("Failed to begin render pass, ignoring draw call.\n");
        return;
    }

    vk_procs = &list->device->vk_procs;

    d3d12_command_list_check_index_buffer_strip_cut_value(list);

    VK_CALL(vkCmdDrawIndexed(list->vk_command_buffer, index_count_per_instance,
            instance_count, start_vertex_location, base_vertex_location, start_instance_location));
}

static void STDMETHODCALLTYPE d3d12_command_list_Dispatch(ID3D12GraphicsCommandList *iface,
        UINT x, UINT y, UINT z)
{
    struct d3d12_command_list *list = impl_from_ID3D12GraphicsCommandList(iface);
    const struct vkd3d_vk_device_procs *vk_procs;

    TRACE("iface %p, x %u, y %u, z %u.\n", iface, x, y, z);

    if (list->state->vk_bind_point != VK_PIPELINE_BIND_POINT_COMPUTE)
    {
        WARN("Pipeline state %p has bind point %#x.\n", list->state, list->state->vk_bind_point);
        return;
    }

    vk_procs = &list->device->vk_procs;

    d3d12_command_list_end_current_render_pass(list);

    d3d12_command_list_update_descriptors(list, VK_PIPELINE_BIND_POINT_COMPUTE);

    VK_CALL(vkCmdDispatch(list->vk_command_buffer, x, y, z));
}

static void STDMETHODCALLTYPE d3d12_command_list_CopyBufferRegion(ID3D12GraphicsCommandList *iface,
        ID3D12Resource *dst, UINT64 dst_offset, ID3D12Resource *src, UINT64 src_offset, UINT64 byte_count)
{
    struct d3d12_command_list *list = impl_from_ID3D12GraphicsCommandList(iface);
    struct d3d12_resource *dst_resource, *src_resource;
    const struct vkd3d_vk_device_procs *vk_procs;
    VkBufferCopy buffer_copy;

    TRACE("iface %p, dst_resource %p, dst_offset %#"PRIx64", src_resource %p, "
            "src_offset %#"PRIx64", byte_count %#"PRIx64".\n",
            iface, dst, dst_offset, src, src_offset, byte_count);

    vk_procs = &list->device->vk_procs;

    dst_resource = unsafe_impl_from_ID3D12Resource(dst);
    assert(d3d12_resource_is_buffer(dst_resource));
    src_resource = unsafe_impl_from_ID3D12Resource(src);
    assert(d3d12_resource_is_buffer(src_resource));

    d3d12_command_list_track_resource_usage(list, dst_resource);
    d3d12_command_list_track_resource_usage(list, src_resource);

    d3d12_command_list_end_current_render_pass(list);

    buffer_copy.srcOffset = src_offset;
    buffer_copy.dstOffset = dst_offset;
    buffer_copy.size = byte_count;

    VK_CALL(vkCmdCopyBuffer(list->vk_command_buffer,
            src_resource->u.vk_buffer, dst_resource->u.vk_buffer, 1, &buffer_copy));
}

static void vk_image_subresource_layers_from_d3d12(VkImageSubresourceLayers *subresource,
        const struct vkd3d_format *format, unsigned int sub_resource_idx, unsigned int miplevel_count)
{
    subresource->aspectMask = format->vk_aspect_mask;
    subresource->mipLevel = sub_resource_idx % miplevel_count;
    subresource->baseArrayLayer = sub_resource_idx / miplevel_count;
    subresource->layerCount = 1;
}

static void vk_extent_3d_from_d3d12_miplevel(VkExtent3D *extent,
        const D3D12_RESOURCE_DESC *resource_desc, unsigned int miplevel_idx)
{
    extent->width = d3d12_resource_desc_get_width(resource_desc, miplevel_idx);
    extent->height = d3d12_resource_desc_get_height(resource_desc, miplevel_idx);
    extent->depth = d3d12_resource_desc_get_depth(resource_desc, miplevel_idx);
}

static void vk_buffer_image_copy_from_d3d12(VkBufferImageCopy *buffer_image_copy,
        const D3D12_PLACED_SUBRESOURCE_FOOTPRINT *footprint, unsigned int sub_resource_idx,
        const D3D12_RESOURCE_DESC *image_desc, const struct vkd3d_format *format,
        unsigned int dst_x, unsigned int dst_y, unsigned int dst_z)
{
    buffer_image_copy->bufferOffset = footprint->Offset;
    buffer_image_copy->bufferRowLength = footprint->Footprint.RowPitch /
            (format->byte_count * format->block_byte_count) * format->block_width;
    buffer_image_copy->bufferImageHeight = 0;
    vk_image_subresource_layers_from_d3d12(&buffer_image_copy->imageSubresource,
            format, sub_resource_idx, image_desc->MipLevels);
    buffer_image_copy->imageOffset.x = dst_x;
    buffer_image_copy->imageOffset.y = dst_y;
    buffer_image_copy->imageOffset.z = dst_z;
    buffer_image_copy->imageExtent.width = footprint->Footprint.Width;
    buffer_image_copy->imageExtent.height = footprint->Footprint.Height;
    buffer_image_copy->imageExtent.depth = footprint->Footprint.Depth;
}

static void vk_image_copy_from_d3d12(VkImageCopy *image_copy,
        unsigned int src_sub_resource_idx, unsigned int dst_sub_resource_idx,
        const D3D12_RESOURCE_DESC *src_desc, const D3D12_RESOURCE_DESC *dst_desc,
        const struct vkd3d_format *src_format, const struct vkd3d_format *dst_format,
        const D3D12_BOX *src_box, unsigned int dst_x, unsigned int dst_y, unsigned int dst_z)
{
    vk_image_subresource_layers_from_d3d12(&image_copy->srcSubresource,
            src_format, src_sub_resource_idx, src_desc->MipLevels);
    image_copy->srcOffset.x = src_box ? src_box->left : 0;
    image_copy->srcOffset.y = src_box ? src_box->top : 0;
    image_copy->srcOffset.z = src_box ? src_box->front : 0;
    vk_image_subresource_layers_from_d3d12(&image_copy->dstSubresource,
            dst_format, dst_sub_resource_idx, dst_desc->MipLevels);
    image_copy->dstOffset.x = dst_x;
    image_copy->dstOffset.y = dst_y;
    image_copy->dstOffset.z = dst_z;
    if (src_box)
    {
        image_copy->extent.width = src_box->right - src_box->left;
        image_copy->extent.height = src_box->bottom - src_box->top;
        image_copy->extent.depth = src_box->back - src_box->front;
    }
    else
    {
        unsigned int miplevel = image_copy->srcSubresource.mipLevel;
        vk_extent_3d_from_d3d12_miplevel(&image_copy->extent, src_desc, miplevel);
    }
}

static HRESULT d3d12_command_list_allocate_transfer_buffer(struct d3d12_command_list *list,
        VkDeviceSize size, struct vkd3d_buffer *buffer)
{
    const struct vkd3d_vk_device_procs *vk_procs = &list->device->vk_procs;
    struct d3d12_device *device = list->device;
    D3D12_HEAP_PROPERTIES heap_properties;
    D3D12_RESOURCE_DESC buffer_desc;
    HRESULT hr;

    memset(&heap_properties, 0, sizeof(heap_properties));
    heap_properties.Type = D3D12_HEAP_TYPE_DEFAULT;

    buffer_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer_desc.Alignment = 0;
    buffer_desc.Width = size;
    buffer_desc.Height = 1;
    buffer_desc.DepthOrArraySize = 1;
    buffer_desc.MipLevels = 1;
    buffer_desc.Format = DXGI_FORMAT_UNKNOWN;
    buffer_desc.SampleDesc.Count = 1;
    buffer_desc.SampleDesc.Quality = 0;
    buffer_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    buffer_desc.Flags = D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;

    if (FAILED(hr = vkd3d_create_buffer(device, &heap_properties, D3D12_HEAP_FLAG_NONE,
            &buffer_desc, &buffer->vk_buffer)))
        return hr;
    if (FAILED(hr = vkd3d_allocate_buffer_memory(device, buffer->vk_buffer,
            &heap_properties, D3D12_HEAP_FLAG_NONE, &buffer->vk_memory)))
    {
        VK_CALL(vkDestroyBuffer(device->vk_device, buffer->vk_buffer, NULL));
        return hr;
    }

    if (!d3d12_command_allocator_add_transfer_buffer(list->allocator, buffer))
    {
        ERR("Failed to add transfer buffer.\n");
        vkd3d_buffer_destroy(buffer, device);
        return E_OUTOFMEMORY;
    }

    return S_OK;
}

/* In Vulkan, each depth/stencil format is only compatible with itself.
 * This means that we are not allowed to copy texture regions directly between
 * depth/stencil and color formats.
 *
 * FIXME: Implement color <-> depth/stencil blits in shaders.
 */
static void d3d12_command_list_copy_incompatible_texture_region(struct d3d12_command_list *list,
        struct d3d12_resource *dst_resource, unsigned int dst_sub_resource_idx,
        const struct vkd3d_format *dst_format, struct d3d12_resource *src_resource,
        unsigned int src_sub_resource_idx, const struct vkd3d_format *src_format)
{
    const struct vkd3d_vk_device_procs *vk_procs = &list->device->vk_procs;
    const D3D12_RESOURCE_DESC *dst_desc = &dst_resource->desc;
    const D3D12_RESOURCE_DESC *src_desc = &src_resource->desc;
    unsigned int dst_miplevel_idx, src_miplevel_idx;
    struct vkd3d_buffer transfer_buffer;
    VkBufferImageCopy buffer_image_copy;
    VkBufferMemoryBarrier vk_barrier;
    VkDeviceSize buffer_size;
    HRESULT hr;

    WARN("Copying incompatible texture formats %#x, %#x -> %#x, %#x.\n",
            src_format->dxgi_format, src_format->vk_format,
            dst_format->dxgi_format, dst_format->vk_format);

    assert(d3d12_resource_is_texture(dst_resource));
    assert(d3d12_resource_is_texture(src_resource));
    assert(!vkd3d_format_is_compressed(dst_format));
    assert(!vkd3d_format_is_compressed(src_format));
    assert(dst_format->byte_count == src_format->byte_count);

    buffer_image_copy.bufferOffset = 0;
    buffer_image_copy.bufferRowLength = 0;
    buffer_image_copy.bufferImageHeight = 0;
    vk_image_subresource_layers_from_d3d12(&buffer_image_copy.imageSubresource,
            src_format, src_sub_resource_idx, src_desc->MipLevels);
    src_miplevel_idx = buffer_image_copy.imageSubresource.mipLevel;
    buffer_image_copy.imageOffset.x = 0;
    buffer_image_copy.imageOffset.y = 0;
    buffer_image_copy.imageOffset.z = 0;
    vk_extent_3d_from_d3d12_miplevel(&buffer_image_copy.imageExtent, src_desc, src_miplevel_idx);

    buffer_size = src_format->byte_count * buffer_image_copy.imageExtent.width *
            buffer_image_copy.imageExtent.height * buffer_image_copy.imageExtent.depth;
    if (FAILED(hr = d3d12_command_list_allocate_transfer_buffer(list, buffer_size, &transfer_buffer)))
    {
        ERR("Failed to allocate transfer buffer, hr %#x.\n", hr);
        return;
    }

    VK_CALL(vkCmdCopyImageToBuffer(list->vk_command_buffer,
            src_resource->u.vk_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            transfer_buffer.vk_buffer, 1, &buffer_image_copy));

    vk_barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    vk_barrier.pNext = NULL;
    vk_barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vk_barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    vk_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    vk_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    vk_barrier.buffer = transfer_buffer.vk_buffer;
    vk_barrier.offset = 0;
    vk_barrier.size = VK_WHOLE_SIZE;
    VK_CALL(vkCmdPipelineBarrier(list->vk_command_buffer,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
            0, NULL, 1, &vk_barrier, 0, NULL));

    vk_image_subresource_layers_from_d3d12(&buffer_image_copy.imageSubresource,
            dst_format, dst_sub_resource_idx, dst_desc->MipLevels);
    dst_miplevel_idx = buffer_image_copy.imageSubresource.mipLevel;

    assert(d3d12_resource_desc_get_width(src_desc, src_miplevel_idx) ==
            d3d12_resource_desc_get_width(dst_desc, dst_miplevel_idx));
    assert(d3d12_resource_desc_get_height(src_desc, src_miplevel_idx) ==
            d3d12_resource_desc_get_height(dst_desc, dst_miplevel_idx));
    assert(d3d12_resource_desc_get_depth(src_desc, src_miplevel_idx) ==
            d3d12_resource_desc_get_depth(dst_desc, dst_miplevel_idx));

    VK_CALL(vkCmdCopyBufferToImage(list->vk_command_buffer,
            transfer_buffer.vk_buffer, dst_resource->u.vk_image,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &buffer_image_copy));
}

static void STDMETHODCALLTYPE d3d12_command_list_CopyTextureRegion(ID3D12GraphicsCommandList *iface,
        const D3D12_TEXTURE_COPY_LOCATION *dst, UINT dst_x, UINT dst_y, UINT dst_z,
        const D3D12_TEXTURE_COPY_LOCATION *src, const D3D12_BOX *src_box)
{
    struct d3d12_command_list *list = impl_from_ID3D12GraphicsCommandList(iface);
    struct d3d12_resource *dst_resource, *src_resource;
    const struct vkd3d_format *src_format, *dst_format;
    const struct vkd3d_vk_device_procs *vk_procs;
    VkBufferImageCopy buffer_image_copy;
    VkImageCopy image_copy;

    TRACE("iface %p, dst %p, dst_x %u, dst_y %u, dst_z %u, src %p, src_box %p.\n",
            iface, dst, dst_x, dst_y, dst_z, src, src_box);

    vk_procs = &list->device->vk_procs;

    dst_resource = unsafe_impl_from_ID3D12Resource(dst->pResource);
    src_resource = unsafe_impl_from_ID3D12Resource(src->pResource);

    d3d12_command_list_track_resource_usage(list, dst_resource);
    d3d12_command_list_track_resource_usage(list, src_resource);

    d3d12_command_list_end_current_render_pass(list);

    if (src->Type == D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX
            && dst->Type == D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT)
    {
        assert(d3d12_resource_is_buffer(dst_resource));
        assert(d3d12_resource_is_texture(src_resource));

        if (!(dst_format = vkd3d_format_from_d3d12_resource_desc(&src_resource->desc,
                dst->u.PlacedFootprint.Footprint.Format)))
        {
            WARN("Invalid format %#x.\n", dst->u.PlacedFootprint.Footprint.Format);
            return;
        }

        if ((dst_format->vk_aspect_mask & VK_IMAGE_ASPECT_DEPTH_BIT)
                && (dst_format->vk_aspect_mask & VK_IMAGE_ASPECT_STENCIL_BIT))
            FIXME("Depth-stencil format %#x not fully supported yet.\n", dst_format->dxgi_format);

        vk_buffer_image_copy_from_d3d12(&buffer_image_copy, &dst->u.PlacedFootprint,
                src->u.SubresourceIndex, &src_resource->desc, dst_format, dst_x, dst_y, dst_z);
        VK_CALL(vkCmdCopyImageToBuffer(list->vk_command_buffer,
                src_resource->u.vk_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                dst_resource->u.vk_buffer, 1, &buffer_image_copy));
    }
    else if (src->Type == D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT
            && dst->Type == D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX)
    {
        assert(d3d12_resource_is_texture(dst_resource));
        assert(d3d12_resource_is_buffer(src_resource));

        if (!(src_format = vkd3d_format_from_d3d12_resource_desc(&dst_resource->desc,
                src->u.PlacedFootprint.Footprint.Format)))
        {
            WARN("Invalid format %#x.\n", src->u.PlacedFootprint.Footprint.Format);
            return;
        }

        if ((src_format->vk_aspect_mask & VK_IMAGE_ASPECT_DEPTH_BIT)
                && (src_format->vk_aspect_mask & VK_IMAGE_ASPECT_STENCIL_BIT))
            FIXME("Depth-stencil format %#x not fully supported yet.\n", src_format->dxgi_format);

        vk_buffer_image_copy_from_d3d12(&buffer_image_copy, &src->u.PlacedFootprint,
                dst->u.SubresourceIndex, &dst_resource->desc, src_format, dst_x, dst_y, dst_z);
        VK_CALL(vkCmdCopyBufferToImage(list->vk_command_buffer,
                src_resource->u.vk_buffer, dst_resource->u.vk_image,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &buffer_image_copy));
    }
    else if (src->Type == D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX
            && dst->Type == D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX)
    {
        assert(d3d12_resource_is_texture(dst_resource));
        assert(d3d12_resource_is_texture(src_resource));

        if (!(dst_format = vkd3d_format_from_d3d12_resource_desc(&dst_resource->desc, DXGI_FORMAT_UNKNOWN)))
        {
            WARN("Invalid format %#x.\n", dst_resource->desc.Format);
            return;
        }
        if (!(src_format = vkd3d_format_from_d3d12_resource_desc(&src_resource->desc, DXGI_FORMAT_UNKNOWN)))
        {
            WARN("Invalid format %#x.\n", src_resource->desc.Format);
            return;
        }

        if ((dst_format->vk_aspect_mask & VK_IMAGE_ASPECT_DEPTH_BIT)
                && (dst_format->vk_aspect_mask & VK_IMAGE_ASPECT_STENCIL_BIT))
            FIXME("Depth-stencil format %#x not fully supported yet.\n", dst_format->dxgi_format);
        if ((src_format->vk_aspect_mask & VK_IMAGE_ASPECT_DEPTH_BIT)
                && (src_format->vk_aspect_mask & VK_IMAGE_ASPECT_STENCIL_BIT))
            FIXME("Depth-stencil format %#x not fully supported yet.\n", src_format->dxgi_format);

        if (dst_format->vk_aspect_mask != src_format->vk_aspect_mask)
        {
            d3d12_command_list_copy_incompatible_texture_region(list,
                    dst_resource, dst->u.SubresourceIndex, dst_format,
                    src_resource, src->u.SubresourceIndex, src_format);
            return;
        }

        vk_image_copy_from_d3d12(&image_copy, src->u.SubresourceIndex, dst->u.SubresourceIndex,
                 &src_resource->desc, &dst_resource->desc, src_format, dst_format,
                 src_box, dst_x, dst_y, dst_z);
        VK_CALL(vkCmdCopyImage(list->vk_command_buffer, src_resource->u.vk_image,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst_resource->u.vk_image,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &image_copy));
    }
    else
    {
        FIXME("Copy type %#x -> %#x not implemented.\n", src->Type, dst->Type);
    }
}

static void STDMETHODCALLTYPE d3d12_command_list_CopyResource(ID3D12GraphicsCommandList *iface,
        ID3D12Resource *dst, ID3D12Resource *src)
{
    struct d3d12_command_list *list = impl_from_ID3D12GraphicsCommandList(iface);
    struct d3d12_resource *dst_resource, *src_resource;
    const struct vkd3d_format *src_format, *dst_format;
    const struct vkd3d_vk_device_procs *vk_procs;
    VkBufferCopy vk_buffer_copy;
    VkImageCopy vk_image_copy;
    unsigned int layer_count;
    unsigned int i;

    TRACE("iface %p, dst_resource %p, src_resource %p.\n", iface, dst, src);

    vk_procs = &list->device->vk_procs;

    dst_resource = unsafe_impl_from_ID3D12Resource(dst);
    src_resource = unsafe_impl_from_ID3D12Resource(src);

    d3d12_command_list_track_resource_usage(list, dst_resource);
    d3d12_command_list_track_resource_usage(list, src_resource);

    d3d12_command_list_end_current_render_pass(list);

    if (d3d12_resource_is_buffer(dst_resource))
    {
        assert(d3d12_resource_is_buffer(src_resource));
        assert(src_resource->desc.Width == dst_resource->desc.Width);

        vk_buffer_copy.srcOffset = 0;
        vk_buffer_copy.dstOffset = 0;
        vk_buffer_copy.size = dst_resource->desc.Width;
        VK_CALL(vkCmdCopyBuffer(list->vk_command_buffer,
                src_resource->u.vk_buffer, dst_resource->u.vk_buffer, 1, &vk_buffer_copy));
    }
    else
    {
        if (!(dst_format = vkd3d_format_from_d3d12_resource_desc(&dst_resource->desc, DXGI_FORMAT_UNKNOWN)))
        {
            WARN("Invalid format %#x.\n", dst_resource->desc.Format);
            return;
        }
        if (!(src_format = vkd3d_format_from_d3d12_resource_desc(&src_resource->desc, DXGI_FORMAT_UNKNOWN)))
        {
            WARN("Invalid format %#x.\n", src_resource->desc.Format);
            return;
        }

        layer_count = d3d12_resource_desc_get_layer_count(&dst_resource->desc);

        assert(d3d12_resource_is_texture(dst_resource));
        assert(d3d12_resource_is_texture(src_resource));
        assert(dst_resource->desc.MipLevels == src_resource->desc.MipLevels);
        assert(layer_count == d3d12_resource_desc_get_layer_count(&src_resource->desc));

        for (i = 0; i < dst_resource->desc.MipLevels; ++i)
        {
            vk_image_copy_from_d3d12(&vk_image_copy, i, i,
                    &src_resource->desc, &dst_resource->desc, src_format, dst_format, NULL, 0, 0, 0);
            vk_image_copy.dstSubresource.layerCount = layer_count;
            vk_image_copy.srcSubresource.layerCount = layer_count;
            VK_CALL(vkCmdCopyImage(list->vk_command_buffer, src_resource->u.vk_image,
                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst_resource->u.vk_image,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &vk_image_copy));
        }
    }
}

static void STDMETHODCALLTYPE d3d12_command_list_CopyTiles(ID3D12GraphicsCommandList *iface,
        ID3D12Resource *tiled_resource, const D3D12_TILED_RESOURCE_COORDINATE *tile_region_start_coordinate,
        const D3D12_TILE_REGION_SIZE *tile_region_size, ID3D12Resource *buffer, UINT64 buffer_offset,
        D3D12_TILE_COPY_FLAGS flags)
{
    FIXME("iface %p, tiled_resource %p, tile_region_start_coordinate %p, tile_region_size %p, "
            "buffer %p, buffer_offset %#"PRIx64", flags %#x stub!\n",
            iface, tiled_resource, tile_region_start_coordinate, tile_region_size,
            buffer, buffer_offset, flags);
}

static void STDMETHODCALLTYPE d3d12_command_list_ResolveSubresource(ID3D12GraphicsCommandList *iface,
        ID3D12Resource *dst, UINT dst_sub_resource_idx,
        ID3D12Resource *src, UINT src_sub_resource_idx, DXGI_FORMAT format)
{
    struct d3d12_command_list *list = impl_from_ID3D12GraphicsCommandList(iface);
    struct d3d12_resource *dst_resource, *src_resource;
    const struct vkd3d_format *src_format, *dst_format;
    const struct vkd3d_vk_device_procs *vk_procs;
    VkImageResolve vk_image_resolve;

    TRACE("iface %p, dst_resource %p, dst_sub_resource_idx %u, src_resource %p, src_sub_resource_idx %u, "
            "format %#x.\n", iface, dst, dst_sub_resource_idx, src, src_sub_resource_idx, format);

    vk_procs = &list->device->vk_procs;

    dst_resource = unsafe_impl_from_ID3D12Resource(dst);
    src_resource = unsafe_impl_from_ID3D12Resource(src);

    assert(d3d12_resource_is_texture(dst_resource));
    assert(d3d12_resource_is_texture(src_resource));

    d3d12_command_list_track_resource_usage(list, dst_resource);
    d3d12_command_list_track_resource_usage(list, src_resource);

    d3d12_command_list_end_current_render_pass(list);

    if (dxgi_format_is_typeless(dst_resource->desc.Format)
            || dxgi_format_is_typeless(src_resource->desc.Format))
    {
        FIXME("Not implemented for typeless resources.\n");
        return;
    }

    if (!(dst_format = vkd3d_format_from_d3d12_resource_desc(&dst_resource->desc, DXGI_FORMAT_UNKNOWN)))
    {
        WARN("Invalid format %#x.\n", dst_resource->desc.Format);
        return;
    }
    if (!(src_format = vkd3d_format_from_d3d12_resource_desc(&src_resource->desc, DXGI_FORMAT_UNKNOWN)))
    {
        WARN("Invalid format %#x.\n", src_resource->desc.Format);
        return;
    }

    /* Resolve of depth/stencil images is not supported in Vulkan. */
    if ((dst_format->vk_aspect_mask & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT))
            || (src_format->vk_aspect_mask & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)))
    {
        FIXME("Resolve of depth/stencil images is not implemented yet.\n");
        return;
    }

    vk_image_subresource_layers_from_d3d12(&vk_image_resolve.srcSubresource,
            src_format, src_sub_resource_idx, src_resource->desc.MipLevels);
    memset(&vk_image_resolve.srcOffset, 0, sizeof(vk_image_resolve.srcOffset));
    vk_image_subresource_layers_from_d3d12(&vk_image_resolve.dstSubresource,
            dst_format, dst_sub_resource_idx, dst_resource->desc.MipLevels);
    memset(&vk_image_resolve.dstOffset, 0, sizeof(vk_image_resolve.dstOffset));
    vk_extent_3d_from_d3d12_miplevel(&vk_image_resolve.extent,
            &dst_resource->desc, vk_image_resolve.dstSubresource.mipLevel);

    VK_CALL(vkCmdResolveImage(list->vk_command_buffer, src_resource->u.vk_image,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst_resource->u.vk_image,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &vk_image_resolve));
}

static enum VkPrimitiveTopology vk_topology_from_d3d12_topology(D3D12_PRIMITIVE_TOPOLOGY topology)
{
    switch (topology)
    {
        case D3D_PRIMITIVE_TOPOLOGY_POINTLIST:
            return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
        case D3D_PRIMITIVE_TOPOLOGY_LINELIST:
            return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
        case D3D_PRIMITIVE_TOPOLOGY_LINESTRIP:
            return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
        case D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST:
            return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        case D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP:
            return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
        default:
            FIXME("Unhandled primitive topology %#x.\n", topology);
            return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
    }
}

static void STDMETHODCALLTYPE d3d12_command_list_IASetPrimitiveTopology(ID3D12GraphicsCommandList *iface,
        D3D12_PRIMITIVE_TOPOLOGY topology)
{
    struct d3d12_command_list *list = impl_from_ID3D12GraphicsCommandList(iface);

    TRACE("iface %p, topology %#x.\n", iface, topology);

    if (topology == D3D_PRIMITIVE_TOPOLOGY_UNDEFINED)
    {
        WARN("Ignoring D3D_PRIMITIVE_TOPOLOGY_UNDEFINED.\n");
        return;
    }

    list->primitive_topology = vk_topology_from_d3d12_topology(topology);
    d3d12_command_list_invalidate_current_pipeline(list);
}

static void STDMETHODCALLTYPE d3d12_command_list_RSSetViewports(ID3D12GraphicsCommandList *iface,
        UINT viewport_count, const D3D12_VIEWPORT *viewports)
{
    VkViewport vk_viewports[D3D12_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
    struct d3d12_command_list *list = impl_from_ID3D12GraphicsCommandList(iface);
    const struct vkd3d_vk_device_procs *vk_procs;
    unsigned int i;

    TRACE("iface %p, viewport_count %u, viewports %p.\n", iface, viewport_count, viewports);

    assert(viewport_count <= D3D12_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE);

    for (i = 0; i < viewport_count; ++i)
    {
        vk_viewports[i].x = viewports[i].TopLeftX;
        vk_viewports[i].y = viewports[i].TopLeftY + viewports[i].Height;
        vk_viewports[i].width = viewports[i].Width;
        vk_viewports[i].height = -viewports[i].Height;
        vk_viewports[i].minDepth = viewports[i].MinDepth;
        vk_viewports[i].maxDepth = viewports[i].MaxDepth;
    }

    vk_procs = &list->device->vk_procs;
    VK_CALL(vkCmdSetViewport(list->vk_command_buffer, 0, viewport_count, vk_viewports));
}

static void STDMETHODCALLTYPE d3d12_command_list_RSSetScissorRects(ID3D12GraphicsCommandList *iface,
        UINT rect_count, const D3D12_RECT *rects)
{
    struct d3d12_command_list *list = impl_from_ID3D12GraphicsCommandList(iface);
    const struct vkd3d_vk_device_procs *vk_procs;
    struct VkRect2D *vk_rects;
    unsigned int i;

    TRACE("iface %p, rect_count %u, rects %p.\n", iface, rect_count, rects);

    if (!(vk_rects = vkd3d_calloc(rect_count, sizeof(*vk_rects))))
    {
        ERR("Failed to allocate Vulkan scissor rects.\n");
        return;
    }

    for (i = 0; i < rect_count; ++i)
    {
        vk_rects[i].offset.x = rects[i].left;
        vk_rects[i].offset.y = rects[i].top;
        vk_rects[i].extent.width = rects[i].right - rects[i].left;
        vk_rects[i].extent.height = rects[i].bottom - rects[i].top;
    }

    vk_procs = &list->device->vk_procs;
    VK_CALL(vkCmdSetScissor(list->vk_command_buffer, 0, rect_count, vk_rects));

    free(vk_rects);
}

static void STDMETHODCALLTYPE d3d12_command_list_OMSetBlendFactor(ID3D12GraphicsCommandList *iface,
        const FLOAT blend_factor[4])
{
    struct d3d12_command_list *list = impl_from_ID3D12GraphicsCommandList(iface);
    const struct vkd3d_vk_device_procs *vk_procs;

    TRACE("iface %p, blend_factor %p.\n", iface, blend_factor);

    vk_procs = &list->device->vk_procs;
    VK_CALL(vkCmdSetBlendConstants(list->vk_command_buffer, blend_factor));
}

static void STDMETHODCALLTYPE d3d12_command_list_OMSetStencilRef(ID3D12GraphicsCommandList *iface,
        UINT stencil_ref)
{
    struct d3d12_command_list *list = impl_from_ID3D12GraphicsCommandList(iface);
    const struct vkd3d_vk_device_procs *vk_procs;

    TRACE("iface %p, stencil_ref %u.\n", iface, stencil_ref);

    vk_procs = &list->device->vk_procs;
    VK_CALL(vkCmdSetStencilReference(list->vk_command_buffer, VK_STENCIL_FRONT_AND_BACK, stencil_ref));
}

static void STDMETHODCALLTYPE d3d12_command_list_SetPipelineState(ID3D12GraphicsCommandList *iface,
        ID3D12PipelineState *pipeline_state)
{
    struct d3d12_pipeline_state *state = unsafe_impl_from_ID3D12PipelineState(pipeline_state);
    struct d3d12_command_list *list = impl_from_ID3D12GraphicsCommandList(iface);
    const struct vkd3d_vk_device_procs *vk_procs;

    TRACE("iface %p, pipeline_state %p.\n", iface, pipeline_state);

    vk_procs = &list->device->vk_procs;

    d3d12_command_list_invalidate_bindings(list, state);

    if (d3d12_pipeline_state_is_compute(state))
    {
        VK_CALL(vkCmdBindPipeline(list->vk_command_buffer, state->vk_bind_point, state->u.compute.vk_pipeline));
    }
    else
    {
        if (!d3d12_pipeline_state_is_render_pass_compatible(list->state, state))
        {
            d3d12_command_list_invalidate_current_framebuffer(list);
            d3d12_command_list_invalidate_current_render_pass(list);
        }
        d3d12_command_list_invalidate_current_pipeline(list);
    }

    list->state = state;
}

static void STDMETHODCALLTYPE d3d12_command_list_ResourceBarrier(ID3D12GraphicsCommandList *iface,
        UINT barrier_count, const D3D12_RESOURCE_BARRIER *barriers)
{
    struct d3d12_command_list *list = impl_from_ID3D12GraphicsCommandList(iface);
    bool have_aliasing_barriers = false, have_split_barriers = false;
    const struct vkd3d_vk_device_procs *vk_procs;
    unsigned int i;

    TRACE("iface %p, barrier_count %u, barriers %p.\n", iface, barrier_count, barriers);

    vk_procs = &list->device->vk_procs;

    d3d12_command_list_end_current_render_pass(list);

    for (i = 0; i < barrier_count; ++i)
    {
        unsigned int sub_resource_idx = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        VkPipelineStageFlags src_stage_mask = 0, dst_stage_mask = 0;
        VkAccessFlags src_access_mask = 0, dst_access_mask = 0;
        const D3D12_RESOURCE_BARRIER *current = &barriers[i];
        VkImageLayout layout_before, layout_after;
        struct d3d12_resource *resource;

        have_split_barriers = have_split_barriers
                || (current->Flags & D3D12_RESOURCE_BARRIER_FLAG_BEGIN_ONLY)
                || (current->Flags & D3D12_RESOURCE_BARRIER_FLAG_END_ONLY);

        if (current->Flags & D3D12_RESOURCE_BARRIER_FLAG_BEGIN_ONLY)
            continue;

        switch (current->Type)
        {
            case D3D12_RESOURCE_BARRIER_TYPE_TRANSITION:
            {
                const D3D12_RESOURCE_TRANSITION_BARRIER *transition = &current->u.Transition;

                if (!is_valid_resource_state(transition->StateBefore))
                {
                    WARN("Invalid StateBefore %#x (barrier %u).\n", transition->StateBefore, i);
                    list->is_valid = false;
                    continue;
                }
                if (!is_valid_resource_state(transition->StateAfter))
                {
                    WARN("Invalid StateAfter %#x (barrier %u).\n", transition->StateAfter, i);
                    list->is_valid = false;
                    continue;
                }

                resource = unsafe_impl_from_ID3D12Resource(transition->pResource);
                assert(resource);
                sub_resource_idx = transition->Subresource;

                if (!vk_barrier_parameters_from_d3d12_resource_state(transition->StateBefore,
                        resource->flags & VKD3D_RESOURCE_PRESENT_STATE_TRANSITION, resource->present_state,
                        list->vk_queue_flags, &src_access_mask, &src_stage_mask, &layout_before))
                {
                    FIXME("Unhandled state %#x.\n", transition->StateBefore);
                    continue;
                }
                if (!vk_barrier_parameters_from_d3d12_resource_state(transition->StateAfter,
                        resource->flags & VKD3D_RESOURCE_PRESENT_STATE_TRANSITION, resource->present_state,
                        list->vk_queue_flags, &dst_access_mask, &dst_stage_mask, &layout_after))
                {
                    FIXME("Unhandled state %#x.\n", transition->StateAfter);
                    continue;
                }

                TRACE("Transition barrier (resource %p, subresource %#x, before %#x, after %#x).\n",
                        resource, transition->Subresource, transition->StateBefore, transition->StateAfter);
                break;
            }

            case D3D12_RESOURCE_BARRIER_TYPE_UAV:
            {
                const D3D12_RESOURCE_UAV_BARRIER *uav = &current->u.UAV;
                VkPipelineStageFlags stage_mask;
                VkImageLayout image_layout;
                VkAccessFlags access_mask;

                resource = unsafe_impl_from_ID3D12Resource(uav->pResource);
                vk_barrier_parameters_from_d3d12_resource_state(D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                        resource && (resource->flags & VKD3D_RESOURCE_PRESENT_STATE_TRANSITION),
                        resource ? resource->present_state : 0, list->vk_queue_flags,
                        &access_mask, &stage_mask, &image_layout);
                src_access_mask = dst_access_mask = access_mask;
                src_stage_mask = dst_stage_mask = stage_mask;
                layout_before = layout_after = image_layout;

                TRACE("UAV barrier (resource %p).\n", resource);
                break;
            }

            case D3D12_RESOURCE_BARRIER_TYPE_ALIASING:
                have_aliasing_barriers = true;
                continue;
            default:
                WARN("Invalid barrier type %#x.\n", current->Type);
                continue;
        }

        if (resource)
            d3d12_command_list_track_resource_usage(list, resource);

        if (!resource)
        {
            VkMemoryBarrier vk_barrier;

            vk_barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
            vk_barrier.pNext = NULL;
            vk_barrier.srcAccessMask = src_access_mask;
            vk_barrier.dstAccessMask = dst_access_mask;

            VK_CALL(vkCmdPipelineBarrier(list->vk_command_buffer, src_stage_mask, dst_stage_mask, 0,
                    1, &vk_barrier, 0, NULL, 0, NULL));
        }
        else if (d3d12_resource_is_buffer(resource))
        {
            VkBufferMemoryBarrier vk_barrier;

            vk_barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
            vk_barrier.pNext = NULL;
            vk_barrier.srcAccessMask = src_access_mask;
            vk_barrier.dstAccessMask = dst_access_mask;
            vk_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            vk_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            vk_barrier.buffer = resource->u.vk_buffer;
            vk_barrier.offset = 0;
            vk_barrier.size = VK_WHOLE_SIZE;

            VK_CALL(vkCmdPipelineBarrier(list->vk_command_buffer, src_stage_mask, dst_stage_mask, 0,
                    0, NULL, 1, &vk_barrier, 0, NULL));
        }
        else
        {
            const struct vkd3d_format *format;
            VkImageMemoryBarrier vk_barrier;

            if (!(format = vkd3d_format_from_d3d12_resource_desc(&resource->desc, 0)))
            {
                ERR("Resource %p has invalid format %#x.\n", resource, resource->desc.Format);
                continue;
            }

            vk_barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            vk_barrier.pNext = NULL;
            vk_barrier.srcAccessMask = src_access_mask;
            vk_barrier.dstAccessMask = dst_access_mask;
            vk_barrier.oldLayout = layout_before;
            vk_barrier.newLayout = layout_after;
            vk_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            vk_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            vk_barrier.image = resource->u.vk_image;

            vk_barrier.subresourceRange.aspectMask = format->vk_aspect_mask;
            if (sub_resource_idx == D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES)
            {
                vk_barrier.subresourceRange.baseMipLevel = 0;
                vk_barrier.subresourceRange.levelCount = VK_REMAINING_MIP_LEVELS;
                vk_barrier.subresourceRange.baseArrayLayer = 0;
                vk_barrier.subresourceRange.layerCount = VK_REMAINING_ARRAY_LAYERS;
            }
            else
            {
                vk_barrier.subresourceRange.baseMipLevel = sub_resource_idx % resource->desc.MipLevels;
                vk_barrier.subresourceRange.levelCount = 1;
                vk_barrier.subresourceRange.baseArrayLayer = sub_resource_idx / resource->desc.MipLevels;
                vk_barrier.subresourceRange.layerCount = 1;
            }

            VK_CALL(vkCmdPipelineBarrier(list->vk_command_buffer, src_stage_mask, dst_stage_mask, 0,
                    0, NULL, 0, NULL, 1, &vk_barrier));
        }
    }

    if (have_aliasing_barriers)
        FIXME("Aliasing barriers not implemented yet.\n");

    /* Vulkan doesn't support split barriers. */
    if (have_split_barriers)
        WARN("Issuing split barrier(s) on D3D12_RESOURCE_BARRIER_FLAG_END_ONLY.\n");
}

static void STDMETHODCALLTYPE d3d12_command_list_ExecuteBundle(ID3D12GraphicsCommandList *iface,
        ID3D12GraphicsCommandList *command_list)
{
    FIXME("iface %p, command_list %p stub!\n", iface, command_list);
}

static void STDMETHODCALLTYPE d3d12_command_list_SetDescriptorHeaps(ID3D12GraphicsCommandList *iface,
        UINT heap_count, ID3D12DescriptorHeap *const *heaps)
{
    TRACE("iface %p, heap_count %u, heaps %p.\n", iface, heap_count, heaps);

    /* Our current implementation does not need this method.
     *
     * It could be used to validate descriptor tables but we do not have an
     * equivalent of the D3D12 Debug Layer. */
}

static void d3d12_command_list_set_root_signature(struct d3d12_command_list *list,
        VkPipelineBindPoint bind_point, const struct d3d12_root_signature *root_signature)
{
    struct vkd3d_pipeline_bindings *bindings = &list->pipeline_bindings[bind_point];

    if (bindings->root_signature == root_signature)
        return;

    bindings->root_signature = root_signature;
    bindings->descriptor_set = VK_NULL_HANDLE;
    bindings->descriptor_table_active_mask = 0;
    bindings->push_descriptor_active_mask = 0;
}

static void STDMETHODCALLTYPE d3d12_command_list_SetComputeRootSignature(ID3D12GraphicsCommandList *iface,
        ID3D12RootSignature *root_signature)
{
    struct d3d12_command_list *list = impl_from_ID3D12GraphicsCommandList(iface);

    TRACE("iface %p, root_signature %p.\n", iface, root_signature);

    d3d12_command_list_set_root_signature(list, VK_PIPELINE_BIND_POINT_COMPUTE,
            unsafe_impl_from_ID3D12RootSignature(root_signature));
}

static void STDMETHODCALLTYPE d3d12_command_list_SetGraphicsRootSignature(ID3D12GraphicsCommandList *iface,
        ID3D12RootSignature *root_signature)
{
    struct d3d12_command_list *list = impl_from_ID3D12GraphicsCommandList(iface);

    TRACE("iface %p, root_signature %p.\n", iface, root_signature);

    d3d12_command_list_set_root_signature(list, VK_PIPELINE_BIND_POINT_GRAPHICS,
            unsafe_impl_from_ID3D12RootSignature(root_signature));
}

static void d3d12_command_list_set_descriptor_table(struct d3d12_command_list *list,
        VkPipelineBindPoint bind_point, unsigned int index, D3D12_GPU_DESCRIPTOR_HANDLE base_descriptor)
{
    struct vkd3d_pipeline_bindings *bindings = &list->pipeline_bindings[bind_point];
    const struct d3d12_root_signature *root_signature = bindings->root_signature;

    assert(root_signature->parameters[index].parameter_type == D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE);

    assert(index < ARRAY_SIZE(bindings->descriptor_tables));
    bindings->descriptor_tables[index] = base_descriptor;
    bindings->descriptor_table_dirty_mask |= (uint64_t)1 << index;
    bindings->descriptor_table_active_mask |= (uint64_t)1 << index;
}

static void STDMETHODCALLTYPE d3d12_command_list_SetComputeRootDescriptorTable(ID3D12GraphicsCommandList *iface,
        UINT root_parameter_index, D3D12_GPU_DESCRIPTOR_HANDLE base_descriptor)
{
    struct d3d12_command_list *list = impl_from_ID3D12GraphicsCommandList(iface);

    TRACE("iface %p, root_parameter_index %u, base_descriptor %#"PRIx64".\n",
            iface, root_parameter_index, base_descriptor.ptr);

    d3d12_command_list_set_descriptor_table(list, VK_PIPELINE_BIND_POINT_COMPUTE,
            root_parameter_index, base_descriptor);
}

static void STDMETHODCALLTYPE d3d12_command_list_SetGraphicsRootDescriptorTable(ID3D12GraphicsCommandList *iface,
        UINT root_parameter_index, D3D12_GPU_DESCRIPTOR_HANDLE base_descriptor)
{
    struct d3d12_command_list *list = impl_from_ID3D12GraphicsCommandList(iface);

    TRACE("iface %p, root_parameter_index %u, base_descriptor %#"PRIx64".\n",
            iface, root_parameter_index, base_descriptor.ptr);

    d3d12_command_list_set_descriptor_table(list, VK_PIPELINE_BIND_POINT_GRAPHICS,
            root_parameter_index, base_descriptor);
}

static void d3d12_command_list_set_root_constants(struct d3d12_command_list *list,
        VkPipelineBindPoint bind_point, unsigned int index, unsigned int offset,
        unsigned int count, const void *data)
{
    const struct d3d12_root_signature *root_signature = list->pipeline_bindings[bind_point].root_signature;
    const struct vkd3d_vk_device_procs *vk_procs = &list->device->vk_procs;
    const struct d3d12_root_constant *c;

    assert(root_signature->parameters[index].parameter_type == D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS);
    c = &root_signature->parameters[index].u.constant;
    VK_CALL(vkCmdPushConstants(list->vk_command_buffer, root_signature->vk_pipeline_layout,
            c->stage_flags, c->offset + offset * sizeof(uint32_t), count * sizeof(uint32_t), data));
}

static void STDMETHODCALLTYPE d3d12_command_list_SetComputeRoot32BitConstant(ID3D12GraphicsCommandList *iface,
        UINT root_parameter_index, UINT data, UINT dst_offset)
{
    struct d3d12_command_list *list = impl_from_ID3D12GraphicsCommandList(iface);

    TRACE("iface %p, root_parameter_index %u, data 0x%08x, dst_offset %u.\n",
            iface, root_parameter_index, data, dst_offset);

    d3d12_command_list_set_root_constants(list, VK_PIPELINE_BIND_POINT_COMPUTE,
            root_parameter_index, dst_offset, 1, &data);
}

static void STDMETHODCALLTYPE d3d12_command_list_SetGraphicsRoot32BitConstant(ID3D12GraphicsCommandList *iface,
        UINT root_parameter_index, UINT data, UINT dst_offset)
{
    struct d3d12_command_list *list = impl_from_ID3D12GraphicsCommandList(iface);

    TRACE("iface %p, root_parameter_index %u, data 0x%08x, dst_offset %u.\n",
            iface, root_parameter_index, data, dst_offset);

    d3d12_command_list_set_root_constants(list, VK_PIPELINE_BIND_POINT_GRAPHICS,
            root_parameter_index, dst_offset, 1, &data);
}

static void STDMETHODCALLTYPE d3d12_command_list_SetComputeRoot32BitConstants(ID3D12GraphicsCommandList *iface,
        UINT root_parameter_index, UINT constant_count, const void *data, UINT dst_offset)
{
    struct d3d12_command_list *list = impl_from_ID3D12GraphicsCommandList(iface);

    TRACE("iface %p, root_parameter_index %u, constant_count %u, data %p, dst_offset %u.\n",
            iface, root_parameter_index, constant_count, data, dst_offset);

    d3d12_command_list_set_root_constants(list, VK_PIPELINE_BIND_POINT_COMPUTE,
            root_parameter_index, dst_offset, constant_count, data);
}

static void STDMETHODCALLTYPE d3d12_command_list_SetGraphicsRoot32BitConstants(ID3D12GraphicsCommandList *iface,
        UINT root_parameter_index, UINT constant_count, const void *data, UINT dst_offset)
{
    struct d3d12_command_list *list = impl_from_ID3D12GraphicsCommandList(iface);

    TRACE("iface %p, root_parameter_index %u, constant_count %u, data %p, dst_offset %u.\n",
            iface, root_parameter_index, constant_count, data, dst_offset);

    d3d12_command_list_set_root_constants(list, VK_PIPELINE_BIND_POINT_GRAPHICS,
            root_parameter_index, dst_offset, constant_count, data);
}

static void d3d12_command_list_set_root_cbv(struct d3d12_command_list *list,
        VkPipelineBindPoint bind_point, unsigned int index, D3D12_GPU_VIRTUAL_ADDRESS gpu_address)
{
    struct vkd3d_pipeline_bindings *bindings = &list->pipeline_bindings[bind_point];
    const struct d3d12_root_signature *root_signature = bindings->root_signature;
    const struct vkd3d_vk_device_procs *vk_procs = &list->device->vk_procs;
    const struct vkd3d_vulkan_info *vk_info = &list->device->vk_info;
    const struct d3d12_root_parameter *root_parameter;
    struct VkWriteDescriptorSet descriptor_write;
    struct VkDescriptorBufferInfo buffer_info;
    struct d3d12_resource *resource;

    root_parameter = &root_signature->parameters[index];
    assert(root_parameter->parameter_type == D3D12_ROOT_PARAMETER_TYPE_CBV);

    resource = vkd3d_gpu_va_allocator_dereference(&list->device->gpu_va_allocator, gpu_address);
    buffer_info.buffer = resource->u.vk_buffer;
    buffer_info.offset = gpu_address - resource->gpu_address;
    buffer_info.range = VK_WHOLE_SIZE;

    if (vk_info->KHR_push_descriptor)
    {
        vk_write_descriptor_set_from_root_descriptor(&descriptor_write,
                root_parameter, VK_NULL_HANDLE, NULL, &buffer_info);
        VK_CALL(vkCmdPushDescriptorSetKHR(list->vk_command_buffer, bind_point,
                root_signature->vk_pipeline_layout, 0, 1, &descriptor_write));
    }
    else
    {
        d3d12_command_list_prepare_descriptors(list, bind_point);
        vk_write_descriptor_set_from_root_descriptor(&descriptor_write,
                root_parameter, bindings->descriptor_set, NULL, &buffer_info);
        VK_CALL(vkUpdateDescriptorSets(list->device->vk_device, 1, &descriptor_write, 0, NULL));

        assert(index < ARRAY_SIZE(bindings->push_descriptors));
        bindings->push_descriptors[index].u.cbv.vk_buffer = buffer_info.buffer;
        bindings->push_descriptors[index].u.cbv.offset = buffer_info.offset;
        bindings->push_descriptor_dirty_mask |= 1u << index;
        bindings->push_descriptor_active_mask |= 1u << index;
    }
}

static void STDMETHODCALLTYPE d3d12_command_list_SetComputeRootConstantBufferView(
        ID3D12GraphicsCommandList *iface, UINT root_parameter_index, D3D12_GPU_VIRTUAL_ADDRESS address)
{
    struct d3d12_command_list *list = impl_from_ID3D12GraphicsCommandList(iface);

    TRACE("iface %p, root_parameter_index %u, address %#"PRIx64".\n",
            iface, root_parameter_index, address);

    d3d12_command_list_set_root_cbv(list, VK_PIPELINE_BIND_POINT_COMPUTE, root_parameter_index, address);
}

static void STDMETHODCALLTYPE d3d12_command_list_SetGraphicsRootConstantBufferView(
        ID3D12GraphicsCommandList *iface, UINT root_parameter_index, D3D12_GPU_VIRTUAL_ADDRESS address)
{
    struct d3d12_command_list *list = impl_from_ID3D12GraphicsCommandList(iface);

    TRACE("iface %p, root_parameter_index %u, address %#"PRIx64".\n",
            iface, root_parameter_index, address);

    d3d12_command_list_set_root_cbv(list, VK_PIPELINE_BIND_POINT_GRAPHICS, root_parameter_index, address);
}

static void d3d12_command_list_set_root_descriptor(struct d3d12_command_list *list,
        VkPipelineBindPoint bind_point, unsigned int index, D3D12_GPU_VIRTUAL_ADDRESS gpu_address)
{
    struct vkd3d_pipeline_bindings *bindings = &list->pipeline_bindings[bind_point];
    const struct d3d12_root_signature *root_signature = bindings->root_signature;
    const struct vkd3d_vk_device_procs *vk_procs = &list->device->vk_procs;
    const struct vkd3d_vulkan_info *vk_info = &list->device->vk_info;
    const struct d3d12_root_parameter *root_parameter;
    struct VkWriteDescriptorSet descriptor_write;
    VkDevice vk_device = list->device->vk_device;
    VkBufferView vk_buffer_view;

    root_parameter = &root_signature->parameters[index];
    assert(root_parameter->parameter_type == D3D12_ROOT_PARAMETER_TYPE_SRV
        || root_parameter->parameter_type == D3D12_ROOT_PARAMETER_TYPE_UAV);

    /* FIXME: Re-use buffer views. */
    if (!vkd3d_create_raw_buffer_view(list->device, gpu_address, &vk_buffer_view))
    {
        ERR("Failed to create buffer view.\n");
        return;
    }

    if (!(d3d12_command_allocator_add_buffer_view(list->allocator, vk_buffer_view)))
    {
        ERR("Failed to add buffer view.\n");
        VK_CALL(vkDestroyBufferView(vk_device, vk_buffer_view, NULL));
        return;
    }

    if (vk_info->KHR_push_descriptor)
    {
        vk_write_descriptor_set_from_root_descriptor(&descriptor_write,
                root_parameter, VK_NULL_HANDLE, &vk_buffer_view, NULL);
        VK_CALL(vkCmdPushDescriptorSetKHR(list->vk_command_buffer, bind_point,
                root_signature->vk_pipeline_layout, 0, 1, &descriptor_write));
    }
    else
    {
        d3d12_command_list_prepare_descriptors(list, bind_point);
        vk_write_descriptor_set_from_root_descriptor(&descriptor_write,
                root_parameter, bindings->descriptor_set, &vk_buffer_view,  NULL);
        VK_CALL(vkUpdateDescriptorSets(list->device->vk_device, 1, &descriptor_write, 0, NULL));

        assert(index < ARRAY_SIZE(bindings->push_descriptors));
        bindings->push_descriptors[index].u.vk_buffer_view = vk_buffer_view;
        bindings->push_descriptor_dirty_mask |= 1u << index;
        bindings->push_descriptor_active_mask |= 1u << index;
    }
}

static void STDMETHODCALLTYPE d3d12_command_list_SetComputeRootShaderResourceView(
        ID3D12GraphicsCommandList *iface, UINT root_parameter_index, D3D12_GPU_VIRTUAL_ADDRESS address)
{
    struct d3d12_command_list *list = impl_from_ID3D12GraphicsCommandList(iface);

    TRACE("iface %p, root_parameter_index %u, address %#"PRIx64".\n",
            iface, root_parameter_index, address);

    d3d12_command_list_set_root_descriptor(list, VK_PIPELINE_BIND_POINT_COMPUTE,
            root_parameter_index, address);
}

static void STDMETHODCALLTYPE d3d12_command_list_SetGraphicsRootShaderResourceView(
        ID3D12GraphicsCommandList *iface, UINT root_parameter_index, D3D12_GPU_VIRTUAL_ADDRESS address)
{
    struct d3d12_command_list *list = impl_from_ID3D12GraphicsCommandList(iface);

    TRACE("iface %p, root_parameter_index %u, address %#"PRIx64".\n",
            iface, root_parameter_index, address);

    d3d12_command_list_set_root_descriptor(list, VK_PIPELINE_BIND_POINT_GRAPHICS,
            root_parameter_index, address);
}

static void STDMETHODCALLTYPE d3d12_command_list_SetComputeRootUnorderedAccessView(
        ID3D12GraphicsCommandList *iface, UINT root_parameter_index, D3D12_GPU_VIRTUAL_ADDRESS address)
{
    struct d3d12_command_list *list = impl_from_ID3D12GraphicsCommandList(iface);

    TRACE("iface %p, root_parameter_index %u, address %#"PRIx64".\n",
            iface, root_parameter_index, address);

    d3d12_command_list_set_root_descriptor(list, VK_PIPELINE_BIND_POINT_COMPUTE,
            root_parameter_index, address);
}

static void STDMETHODCALLTYPE d3d12_command_list_SetGraphicsRootUnorderedAccessView(
        ID3D12GraphicsCommandList *iface, UINT root_parameter_index, D3D12_GPU_VIRTUAL_ADDRESS address)
{
    struct d3d12_command_list *list = impl_from_ID3D12GraphicsCommandList(iface);

    TRACE("iface %p, root_parameter_index %u, address %#"PRIx64".\n",
            iface, root_parameter_index, address);

    d3d12_command_list_set_root_descriptor(list, VK_PIPELINE_BIND_POINT_GRAPHICS,
            root_parameter_index, address);
}

static void STDMETHODCALLTYPE d3d12_command_list_IASetIndexBuffer(ID3D12GraphicsCommandList *iface,
        const D3D12_INDEX_BUFFER_VIEW *view)
{
    struct d3d12_command_list *list = impl_from_ID3D12GraphicsCommandList(iface);
    const struct vkd3d_vk_device_procs *vk_procs;
    struct d3d12_resource *resource;
    enum VkIndexType index_type;

    TRACE("iface %p, view %p.\n", iface, view);

    vk_procs = &list->device->vk_procs;

    switch (view->Format)
    {
        case DXGI_FORMAT_R16_UINT:
            index_type = VK_INDEX_TYPE_UINT16;
            break;
        case DXGI_FORMAT_R32_UINT:
            index_type = VK_INDEX_TYPE_UINT32;
            break;
        default:
            WARN("Invalid index format %#x.\n", view->Format);
            return;
    }

    list->index_buffer_format = view->Format;

    resource = vkd3d_gpu_va_allocator_dereference(&list->device->gpu_va_allocator, view->BufferLocation);
    VK_CALL(vkCmdBindIndexBuffer(list->vk_command_buffer, resource->u.vk_buffer,
            view->BufferLocation - resource->gpu_address, index_type));
}

static void STDMETHODCALLTYPE d3d12_command_list_IASetVertexBuffers(ID3D12GraphicsCommandList *iface,
        UINT start_slot, UINT view_count, const D3D12_VERTEX_BUFFER_VIEW *views)
{
    struct d3d12_command_list *list = impl_from_ID3D12GraphicsCommandList(iface);
    struct vkd3d_gpu_va_allocator *gpu_va_allocator;
    VkDeviceSize offsets[ARRAY_SIZE(list->strides)];
    const struct vkd3d_vk_device_procs *vk_procs;
    VkBuffer buffers[ARRAY_SIZE(list->strides)];
    unsigned int i, first, count, stride;
    struct d3d12_resource *resource;
    bool invalidate = false;

    TRACE("iface %p, start_slot %u, view_count %u, views %p.\n", iface, start_slot, view_count, views);

    vk_procs = &list->device->vk_procs;
    gpu_va_allocator = &list->device->gpu_va_allocator;

    if (start_slot >= ARRAY_SIZE(list->strides) || view_count > ARRAY_SIZE(list->strides) - start_slot)
    {
        WARN("Invalid start slot %u / view count %u.\n", start_slot, view_count);
        return;
    }

    count = 0;
    first = start_slot;
    for (i = 0; i < view_count; ++i)
    {
        if (views[i].BufferLocation)
        {
            resource = vkd3d_gpu_va_allocator_dereference(gpu_va_allocator, views[i].BufferLocation);
            buffers[count] = resource->u.vk_buffer;
            offsets[count] = views[i].BufferLocation - resource->gpu_address;
            stride = views[i].StrideInBytes;
            ++count;
        }
        else
        {
            if (count)
                VK_CALL(vkCmdBindVertexBuffers(list->vk_command_buffer, first, count, buffers, offsets));
            count = 0;
            first = start_slot + i + 1;

            stride = 0;
        }

        invalidate |= list->strides[start_slot + i] != stride;
        list->strides[start_slot + i] = stride;
    }

    if (count)
        VK_CALL(vkCmdBindVertexBuffers(list->vk_command_buffer, first, count, buffers, offsets));

    if (invalidate)
        d3d12_command_list_invalidate_current_pipeline(list);
}

static void STDMETHODCALLTYPE d3d12_command_list_SOSetTargets(ID3D12GraphicsCommandList *iface,
        UINT start_slot, UINT view_count, const D3D12_STREAM_OUTPUT_BUFFER_VIEW *views)
{
    struct d3d12_command_list *list = impl_from_ID3D12GraphicsCommandList(iface);
    VkDeviceSize offsets[ARRAY_SIZE(list->so_counter_buffers)];
    VkDeviceSize sizes[ARRAY_SIZE(list->so_counter_buffers)];
    VkBuffer buffers[ARRAY_SIZE(list->so_counter_buffers)];
    struct vkd3d_gpu_va_allocator *gpu_va_allocator;
    const struct vkd3d_vk_device_procs *vk_procs;
    struct d3d12_resource *resource;
    unsigned int i, first, count;

    TRACE("iface %p, start_slot %u, view_count %u, views %p.\n", iface, start_slot, view_count, views);

    d3d12_command_list_end_current_render_pass(list);

    if (!list->device->vk_info.EXT_transform_feedback)
    {
        FIXME("Transform feedback is not supported by Vulkan implementation.\n");
        return;
    }

    if (start_slot >= ARRAY_SIZE(buffers) || view_count > ARRAY_SIZE(buffers) - start_slot)
    {
        WARN("Invalid start slot %u / view count %u.\n", start_slot, view_count);
        return;
    }

    vk_procs = &list->device->vk_procs;
    gpu_va_allocator = &list->device->gpu_va_allocator;

    count = 0;
    first = start_slot;
    for (i = 0; i < view_count; ++i)
    {
        if (views[i].BufferLocation && views[i].SizeInBytes)
        {
            resource = vkd3d_gpu_va_allocator_dereference(gpu_va_allocator, views[i].BufferLocation);
            buffers[count] = resource->u.vk_buffer;
            offsets[count] = views[i].BufferLocation - resource->gpu_address;
            sizes[count] = views[i].SizeInBytes;

            resource = vkd3d_gpu_va_allocator_dereference(gpu_va_allocator, views[i].BufferFilledSizeLocation);
            list->so_counter_buffers[start_slot + i] = resource->u.vk_buffer;
            list->so_counter_buffer_offsets[start_slot + i] = views[i].BufferFilledSizeLocation - resource->gpu_address;
            ++count;
        }
        else
        {
            if (count)
                VK_CALL(vkCmdBindTransformFeedbackBuffersEXT(list->vk_command_buffer, first, count, buffers, offsets, sizes));
            count = 0;
            first = start_slot + i + 1;

            list->so_counter_buffers[start_slot + i] = VK_NULL_HANDLE;
            list->so_counter_buffer_offsets[start_slot + i] = 0;
        }
    }

    if (count)
        VK_CALL(vkCmdBindTransformFeedbackBuffersEXT(list->vk_command_buffer, first, count, buffers, offsets, sizes));
}

static void STDMETHODCALLTYPE d3d12_command_list_OMSetRenderTargets(ID3D12GraphicsCommandList *iface,
        UINT render_target_descriptor_count, const D3D12_CPU_DESCRIPTOR_HANDLE *render_target_descriptors,
        BOOL single_descriptor_handle, const D3D12_CPU_DESCRIPTOR_HANDLE *depth_stencil_descriptor)
{
    struct d3d12_command_list *list = impl_from_ID3D12GraphicsCommandList(iface);
    const struct d3d12_rtv_desc *rtv_desc;
    const struct d3d12_dsv_desc *dsv_desc;
    struct vkd3d_view *view;
    unsigned int i;

    TRACE("iface %p, render_target_descriptor_count %u, render_target_descriptors %p, "
            "single_descriptor_handle %#x, depth_stencil_descriptor %p.\n",
            iface, render_target_descriptor_count, render_target_descriptors,
            single_descriptor_handle, depth_stencil_descriptor);

    if (render_target_descriptor_count > ARRAY_SIZE(list->views) - 1)
    {
        WARN("Descriptor count %u > %zu, ignoring extra descriptors.\n",
                render_target_descriptor_count, ARRAY_SIZE(list->views) - 1);
        render_target_descriptor_count = ARRAY_SIZE(list->views) - 1;
    }

    list->fb_width = 0;
    list->fb_height = 0;
    list->fb_layer_count = 0;
    for (i = 0; i < render_target_descriptor_count; ++i)
    {
        if (single_descriptor_handle)
        {
            if ((rtv_desc = d3d12_rtv_desc_from_cpu_handle(*render_target_descriptors)))
                rtv_desc += i;
        }
        else
        {
            rtv_desc = d3d12_rtv_desc_from_cpu_handle(render_target_descriptors[i]);
        }

        if (!rtv_desc || !rtv_desc->resource)
        {
            WARN("RTV descriptor %u is not initialized.\n", i);
            list->views[i + 1] = VK_NULL_HANDLE;
            continue;
        }

        d3d12_command_list_track_resource_usage(list, rtv_desc->resource);

        /* In D3D12 CPU descriptors are consumed when a command is recorded. */
        view = rtv_desc->view;
        if (!d3d12_command_allocator_add_view(list->allocator, view))
        {
            WARN("Failed to add view.\n");
        }

        list->views[i + 1] = view->u.vk_image_view;
        list->fb_width = max(list->fb_width, rtv_desc->width);
        list->fb_height = max(list->fb_height, rtv_desc->height);
        list->fb_layer_count = max(list->fb_layer_count, rtv_desc->layer_count);
    }

    if (depth_stencil_descriptor)
    {
        if ((dsv_desc = d3d12_dsv_desc_from_cpu_handle(*depth_stencil_descriptor))
                && dsv_desc->resource)
        {
            d3d12_command_list_track_resource_usage(list, dsv_desc->resource);

            /* In D3D12 CPU descriptors are consumed when a command is recorded. */
            view = dsv_desc->view;
            if (!d3d12_command_allocator_add_view(list->allocator, view))
            {
                WARN("Failed to add view.\n");
                list->views[0] = VK_NULL_HANDLE;
            }

            list->views[0] = view->u.vk_image_view;
            list->fb_width = max(list->fb_width, dsv_desc->width);
            list->fb_height = max(list->fb_height, dsv_desc->height);
            list->fb_layer_count = max(list->fb_layer_count, dsv_desc->layer_count);
        }
        else
        {
            WARN("DSV descriptor is not initialized.\n");
            list->views[0] = VK_NULL_HANDLE;
        }
    }

    d3d12_command_list_invalidate_current_framebuffer(list);
    d3d12_command_list_invalidate_current_render_pass(list);
}

static void d3d12_command_list_clear(struct d3d12_command_list *list,
        const struct VkAttachmentDescription *attachment_desc,
        const struct VkAttachmentReference *color_reference, const struct VkAttachmentReference *ds_reference,
        struct vkd3d_view *view, size_t width, size_t height, unsigned int layer_count,
        const union VkClearValue *clear_value, unsigned int rect_count, const D3D12_RECT *rects)
{
    const struct vkd3d_vk_device_procs *vk_procs = &list->device->vk_procs;
    struct VkSubpassDescription sub_pass_desc;
    struct VkRenderPassCreateInfo pass_desc;
    struct VkRenderPassBeginInfo begin_desc;
    struct VkFramebufferCreateInfo fb_desc;
    VkFramebuffer vk_framebuffer;
    VkRenderPass vk_render_pass;
    D3D12_RECT full_rect;
    unsigned int i;
    VkResult vr;

    d3d12_command_list_end_current_render_pass(list);

    if (!rect_count)
    {
        full_rect.top = 0;
        full_rect.left = 0;
        full_rect.bottom = height;
        full_rect.right = width;

        rect_count = 1;
        rects = &full_rect;
    }

    sub_pass_desc.flags = 0;
    sub_pass_desc.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub_pass_desc.inputAttachmentCount = 0;
    sub_pass_desc.pInputAttachments = NULL;
    sub_pass_desc.colorAttachmentCount = !!color_reference;
    sub_pass_desc.pColorAttachments = color_reference;
    sub_pass_desc.pResolveAttachments = NULL;
    sub_pass_desc.pDepthStencilAttachment = ds_reference;
    sub_pass_desc.preserveAttachmentCount = 0;
    sub_pass_desc.pPreserveAttachments = NULL;

    pass_desc.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    pass_desc.pNext = NULL;
    pass_desc.flags = 0;
    pass_desc.attachmentCount = 1;
    pass_desc.pAttachments = attachment_desc;
    pass_desc.subpassCount = 1;
    pass_desc.pSubpasses = &sub_pass_desc;
    pass_desc.dependencyCount = 0;
    pass_desc.pDependencies = NULL;
    if ((vr = VK_CALL(vkCreateRenderPass(list->device->vk_device, &pass_desc, NULL, &vk_render_pass))) < 0)
    {
        WARN("Failed to create Vulkan render pass, vr %d.\n", vr);
        return;
    }

    if (!d3d12_command_allocator_add_render_pass(list->allocator, vk_render_pass))
    {
        WARN("Failed to add render pass.\n");
        VK_CALL(vkDestroyRenderPass(list->device->vk_device, vk_render_pass, NULL));
        return;
    }

    if (!d3d12_command_allocator_add_view(list->allocator, view))
    {
        WARN("Failed to add view.\n");
    }

    fb_desc.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fb_desc.pNext = NULL;
    fb_desc.flags = 0;
    fb_desc.renderPass = vk_render_pass;
    fb_desc.attachmentCount = 1;
    fb_desc.pAttachments = &view->u.vk_image_view;
    fb_desc.width = width;
    fb_desc.height = height;
    fb_desc.layers = layer_count;
    if ((vr = VK_CALL(vkCreateFramebuffer(list->device->vk_device, &fb_desc, NULL, &vk_framebuffer))) < 0)
    {
        WARN("Failed to create Vulkan framebuffer, vr %d.\n", vr);
        return;
    }

    if (!d3d12_command_allocator_add_framebuffer(list->allocator, vk_framebuffer))
    {
        WARN("Failed to add framebuffer.\n");
        VK_CALL(vkDestroyFramebuffer(list->device->vk_device, vk_framebuffer, NULL));
        return;
    }

    begin_desc.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    begin_desc.pNext = NULL;
    begin_desc.renderPass = vk_render_pass;
    begin_desc.framebuffer = vk_framebuffer;
    begin_desc.clearValueCount = 1;
    begin_desc.pClearValues = clear_value;

    for (i = 0; i < rect_count; ++i)
    {
        begin_desc.renderArea.offset.x = rects[i].left;
        begin_desc.renderArea.offset.y = rects[i].top;
        begin_desc.renderArea.extent.width = rects[i].right - rects[i].left;
        begin_desc.renderArea.extent.height = rects[i].bottom - rects[i].top;
        VK_CALL(vkCmdBeginRenderPass(list->vk_command_buffer, &begin_desc, VK_SUBPASS_CONTENTS_INLINE));
        VK_CALL(vkCmdEndRenderPass(list->vk_command_buffer));
    }
}

static void STDMETHODCALLTYPE d3d12_command_list_ClearDepthStencilView(ID3D12GraphicsCommandList *iface,
        D3D12_CPU_DESCRIPTOR_HANDLE dsv, D3D12_CLEAR_FLAGS flags, float depth, UINT8 stencil,
        UINT rect_count, const D3D12_RECT *rects)
{
    const union VkClearValue clear_value = {.depthStencil = {depth, stencil}};
    struct d3d12_command_list *list = impl_from_ID3D12GraphicsCommandList(iface);
    const struct d3d12_dsv_desc *dsv_desc = d3d12_dsv_desc_from_cpu_handle(dsv);
    struct VkAttachmentDescription attachment_desc;
    struct VkAttachmentReference ds_reference;

    TRACE("iface %p, dsv %#lx, flags %#x, depth %.8e, stencil 0x%02x, rect_count %u, rects %p.\n",
            iface, dsv.ptr, flags, depth, stencil, rect_count, rects);

    d3d12_command_list_track_resource_usage(list, dsv_desc->resource);

    attachment_desc.flags = 0;
    attachment_desc.format = dsv_desc->format;
    attachment_desc.samples = dsv_desc->sample_count;
    if (flags & D3D12_CLEAR_FLAG_DEPTH)
    {
        attachment_desc.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        attachment_desc.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    }
    else
    {
        attachment_desc.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachment_desc.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    }
    if (flags & D3D12_CLEAR_FLAG_STENCIL)
    {
        attachment_desc.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        attachment_desc.stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
    }
    else
    {
        attachment_desc.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachment_desc.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    }
    attachment_desc.initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    attachment_desc.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    ds_reference.attachment = 0;
    ds_reference.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    d3d12_command_list_clear(list, &attachment_desc, NULL, &ds_reference,
            dsv_desc->view, dsv_desc->width, dsv_desc->height, dsv_desc->layer_count,
            &clear_value, rect_count, rects);
}

static void STDMETHODCALLTYPE d3d12_command_list_ClearRenderTargetView(ID3D12GraphicsCommandList *iface,
        D3D12_CPU_DESCRIPTOR_HANDLE rtv, const FLOAT color[4], UINT rect_count, const D3D12_RECT *rects)
{
    const union VkClearValue clear_value = {{{color[0], color[1], color[2], color[3]}}};
    struct d3d12_command_list *list = impl_from_ID3D12GraphicsCommandList(iface);
    const struct d3d12_rtv_desc *rtv_desc = d3d12_rtv_desc_from_cpu_handle(rtv);
    struct VkAttachmentDescription attachment_desc;
    struct VkAttachmentReference color_reference;

    TRACE("iface %p, rtv %#lx, color %p, rect_count %u, rects %p.\n",
            iface, rtv.ptr, color, rect_count, rects);

    d3d12_command_list_track_resource_usage(list, rtv_desc->resource);

    attachment_desc.flags = 0;
    attachment_desc.format = rtv_desc->format;
    attachment_desc.samples = rtv_desc->sample_count;
    attachment_desc.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachment_desc.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment_desc.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment_desc.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachment_desc.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    attachment_desc.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    color_reference.attachment = 0;
    color_reference.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    d3d12_command_list_clear(list, &attachment_desc, &color_reference, NULL,
            rtv_desc->view, rtv_desc->width, rtv_desc->height, rtv_desc->layer_count,
            &clear_value, rect_count, rects);
}

static void STDMETHODCALLTYPE d3d12_command_list_ClearUnorderedAccessViewUint(ID3D12GraphicsCommandList *iface,
        D3D12_GPU_DESCRIPTOR_HANDLE gpu_handle, D3D12_CPU_DESCRIPTOR_HANDLE cpu_handle, ID3D12Resource *resource,
        const UINT values[4], UINT rect_count, const D3D12_RECT *rects)
{
    struct d3d12_command_list *list = impl_from_ID3D12GraphicsCommandList(iface);
    const struct vkd3d_vk_device_procs *vk_procs;
    const struct d3d12_desc *cpu_descriptor;
    struct d3d12_resource *resource_impl;
    VkImageSubresourceRange range;
    VkClearColorValue color;

    TRACE("iface %p, gpu_handle %#"PRIx64", cpu_handle %lx, resource %p, values %p, rect_count %u, rects %p.\n",
            iface, gpu_handle.ptr, cpu_handle.ptr, resource, values, rect_count, rects);

    vk_procs = &list->device->vk_procs;

    resource_impl = unsafe_impl_from_ID3D12Resource(resource);

    d3d12_command_list_track_resource_usage(list, resource_impl);

    if (rect_count)
    {
        FIXME("Clear rects not supported.\n");
        return;
    }

    d3d12_command_list_end_current_render_pass(list);

    cpu_descriptor = d3d12_desc_from_cpu_handle(cpu_handle);

    if (d3d12_resource_is_buffer(resource_impl))
    {
        if (!cpu_descriptor->uav.buffer.size)
        {
            FIXME("Not supported for UAV descriptor %p.\n", cpu_descriptor);
            return;
        }

        VK_CALL(vkCmdFillBuffer(list->vk_command_buffer, resource_impl->u.vk_buffer,
                cpu_descriptor->uav.buffer.offset, cpu_descriptor->uav.buffer.size, values[0]));
    }
    else
    {
        color.uint32[0] = values[0];
        color.uint32[1] = values[1];
        color.uint32[2] = values[2];
        color.uint32[3] = values[3];

        range.aspectMask = cpu_descriptor->uav.texture.vk_aspect_mask;
        range.baseMipLevel = cpu_descriptor->uav.texture.miplevel_idx;
        range.levelCount = 1;
        range.baseArrayLayer = cpu_descriptor->uav.texture.layer_idx;
        range.layerCount = cpu_descriptor->uav.texture.layer_count;

        VK_CALL(vkCmdClearColorImage(list->vk_command_buffer,
                resource_impl->u.vk_image, VK_IMAGE_LAYOUT_GENERAL, &color, 1, &range));
    }
}

static void STDMETHODCALLTYPE d3d12_command_list_ClearUnorderedAccessViewFloat(ID3D12GraphicsCommandList *iface,
        D3D12_GPU_DESCRIPTOR_HANDLE gpu_handle, D3D12_CPU_DESCRIPTOR_HANDLE cpu_handle, ID3D12Resource *resource,
        const float values[4], UINT rect_count, const D3D12_RECT *rects)
{
    struct d3d12_command_list *list = impl_from_ID3D12GraphicsCommandList(iface);
    struct d3d12_resource *resource_impl;

    FIXME("iface %p, gpu_handle %#"PRIx64", cpu_handle %lx, resource %p, values %p, rect_count %u, rects %p stub!\n",
            iface, gpu_handle.ptr, cpu_handle.ptr, resource, values, rect_count, rects);

    resource_impl = unsafe_impl_from_ID3D12Resource(resource);

    d3d12_command_list_track_resource_usage(list, resource_impl);
}

static void STDMETHODCALLTYPE d3d12_command_list_DiscardResource(ID3D12GraphicsCommandList *iface,
        ID3D12Resource *resource, const D3D12_DISCARD_REGION *region)
{
    FIXME("iface %p, resource %p, region %p stub!\n", iface, resource, region);
}

static void STDMETHODCALLTYPE d3d12_command_list_BeginQuery(ID3D12GraphicsCommandList *iface,
        ID3D12QueryHeap *heap, D3D12_QUERY_TYPE type, UINT index)
{
    struct d3d12_command_list *list = impl_from_ID3D12GraphicsCommandList(iface);
    struct d3d12_query_heap *query_heap = unsafe_impl_from_ID3D12QueryHeap(heap);
    const struct vkd3d_vk_device_procs *vk_procs;
    VkQueryControlFlags flags = 0;

    TRACE("iface %p, heap %p, type %#x, index %u.\n", iface, heap, type, index);

    vk_procs = &list->device->vk_procs;

    d3d12_command_list_end_current_render_pass(list);

    if (type == D3D12_QUERY_TYPE_OCCLUSION)
        flags = VK_QUERY_CONTROL_PRECISE_BIT;

    VK_CALL(vkCmdResetQueryPool(list->vk_command_buffer, query_heap->vk_query_pool, index, 1));
    VK_CALL(vkCmdBeginQuery(list->vk_command_buffer, query_heap->vk_query_pool, index, flags));
}

static void STDMETHODCALLTYPE d3d12_command_list_EndQuery(ID3D12GraphicsCommandList *iface,
        ID3D12QueryHeap *heap, D3D12_QUERY_TYPE type, UINT index)
{
    struct d3d12_command_list *list = impl_from_ID3D12GraphicsCommandList(iface);
    struct d3d12_query_heap *query_heap = unsafe_impl_from_ID3D12QueryHeap(heap);
    const struct vkd3d_vk_device_procs *vk_procs;

    TRACE("iface %p, heap %p, type %#x, index %u.\n", iface, heap, type, index);

    vk_procs = &list->device->vk_procs;

    d3d12_command_list_end_current_render_pass(list);

    d3d12_query_heap_mark_result_as_available(query_heap, index);

    if (type == D3D12_QUERY_TYPE_TIMESTAMP)
    {
        VK_CALL(vkCmdResetQueryPool(list->vk_command_buffer, query_heap->vk_query_pool, index, 1));
        VK_CALL(vkCmdWriteTimestamp(list->vk_command_buffer,
                VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, query_heap->vk_query_pool, index));
        return;
    }

    VK_CALL(vkCmdEndQuery(list->vk_command_buffer, query_heap->vk_query_pool, index));
}

static void STDMETHODCALLTYPE d3d12_command_list_ResolveQueryData(ID3D12GraphicsCommandList *iface,
        ID3D12QueryHeap *heap, D3D12_QUERY_TYPE type, UINT start_index, UINT query_count,
        ID3D12Resource *dst_buffer, UINT64 aligned_dst_buffer_offset)
{
    const struct d3d12_query_heap *query_heap = unsafe_impl_from_ID3D12QueryHeap(heap);
    struct d3d12_command_list *list = impl_from_ID3D12GraphicsCommandList(iface);
    struct d3d12_resource *buffer = unsafe_impl_from_ID3D12Resource(dst_buffer);
    VkDeviceSize offset, stride = sizeof(uint64_t);
    const struct vkd3d_vk_device_procs *vk_procs;
    unsigned int i, first, count;

    TRACE("iface %p, heap %p, type %#x, start_index %u, query_count %u, "
            "dst_buffer %p, aligned_dst_buffer_offset %#"PRIx64".\n",
            iface, heap, type, start_index, query_count,
            dst_buffer, aligned_dst_buffer_offset);

    vk_procs = &list->device->vk_procs;

    /* Vulkan is less strict than D3D12 here. Vulkan implementations are free
     * to return any non-zero result for binary occlusion with at least one
     * sample passing, while D3D12 guarantees that the result is 1 then.
     *
     * For example, the Nvidia binary blob drivers on Linux seem to always
     * count precisely, even when it was signalled that non-precise is enough.
     */
    if (type == D3D12_QUERY_TYPE_BINARY_OCCLUSION)
        FIXME("D3D12 guarantees binary occlusion queries result in only 0 and 1.\n");

    if (!d3d12_resource_is_buffer(buffer))
    {
        WARN("Destination resource is not a buffer.\n");
        return;
    }

    d3d12_command_list_end_current_render_pass(list);

    if (type == D3D12_QUERY_TYPE_PIPELINE_STATISTICS)
        stride = sizeof(struct D3D12_QUERY_DATA_PIPELINE_STATISTICS);

    count = 0;
    first = start_index;
    offset = aligned_dst_buffer_offset;
    for (i = 0; i < query_count; ++i)
    {
        if (d3d12_query_heap_is_result_available(query_heap, start_index + i))
        {
            ++count;
        }
        else
        {
            if (count)
            {
                VK_CALL(vkCmdCopyQueryPoolResults(list->vk_command_buffer,
                        query_heap->vk_query_pool, first, count, buffer->u.vk_buffer,
                        offset, stride, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT));
            }
            count = 0;
            first = start_index + i;
            offset = aligned_dst_buffer_offset + i * stride;

            /* We cannot copy query results if a query was not issued:
             *
             *   "If the query does not become available in a finite amount of
             *   time (e.g. due to not issuing a query since the last reset),
             *   a VK_ERROR_DEVICE_LOST error may occur."
             */
            VK_CALL(vkCmdFillBuffer(list->vk_command_buffer,
                    buffer->u.vk_buffer, offset, stride, 0x00000000));

            ++first;
            offset += stride;
        }
    }

    if (count)
    {
        VK_CALL(vkCmdCopyQueryPoolResults(list->vk_command_buffer,
                query_heap->vk_query_pool, first, count, buffer->u.vk_buffer,
                offset, stride, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT));
    }
}

static void STDMETHODCALLTYPE d3d12_command_list_SetPredication(ID3D12GraphicsCommandList *iface,
        ID3D12Resource *buffer, UINT64 aligned_buffer_offset, D3D12_PREDICATION_OP operation)
{
    FIXME("iface %p, buffer %p, aligned_buffer_offset %#"PRIx64", operation %#x stub!\n",
            iface, buffer, aligned_buffer_offset, operation);
}

static void STDMETHODCALLTYPE d3d12_command_list_SetMarker(ID3D12GraphicsCommandList *iface,
        UINT metadata, const void *data, UINT size)
{
    FIXME("iface %p, metadata %#x, data %p, size %u stub!\n", iface, metadata, data, size);
}

static void STDMETHODCALLTYPE d3d12_command_list_BeginEvent(ID3D12GraphicsCommandList *iface,
        UINT metadata, const void *data, UINT size)
{
    FIXME("iface %p, metadata %#x, data %p, size %u stub!\n", iface, metadata, data, size);
}

static void STDMETHODCALLTYPE d3d12_command_list_EndEvent(ID3D12GraphicsCommandList *iface)
{
    FIXME("iface %p stub!\n", iface);
}

STATIC_ASSERT(sizeof(VkDispatchIndirectCommand) == sizeof(D3D12_DISPATCH_ARGUMENTS));
STATIC_ASSERT(sizeof(VkDrawIndexedIndirectCommand) == sizeof(D3D12_DRAW_INDEXED_ARGUMENTS));
STATIC_ASSERT(sizeof(VkDrawIndirectCommand) == sizeof(D3D12_DRAW_ARGUMENTS));

static void STDMETHODCALLTYPE d3d12_command_list_ExecuteIndirect(ID3D12GraphicsCommandList *iface,
        ID3D12CommandSignature *command_signature, UINT max_command_count, ID3D12Resource *arg_buffer,
        UINT64 arg_buffer_offset, ID3D12Resource *count_buffer, UINT64 count_buffer_offset)
{
    struct d3d12_command_signature *sig_impl = unsafe_impl_from_ID3D12CommandSignature(command_signature);
    struct d3d12_resource *arg_impl = unsafe_impl_from_ID3D12Resource(arg_buffer);
    struct d3d12_command_list *list = impl_from_ID3D12GraphicsCommandList(iface);
    const D3D12_COMMAND_SIGNATURE_DESC *signature_desc;
    const struct vkd3d_vk_device_procs *vk_procs;
    unsigned int i;

    TRACE("iface %p, command_signature %p, max_command_count %u, arg_buffer %p, "
            "arg_buffer_offset %#"PRIx64", count_buffer %p, count_buffer_offset %#"PRIx64".\n",
            iface, command_signature, max_command_count, arg_buffer, arg_buffer_offset,
            count_buffer, count_buffer_offset);

    vk_procs = &list->device->vk_procs;

    if (count_buffer)
    {
        FIXME("Count buffers not implemented.\n");
        return;
    }

    signature_desc = &sig_impl->desc;
    for (i = 0; i < signature_desc->NumArgumentDescs; ++i)
    {
        const D3D12_INDIRECT_ARGUMENT_DESC *arg_desc = &signature_desc->pArgumentDescs[i];

        switch (arg_desc->Type)
        {
            case D3D12_INDIRECT_ARGUMENT_TYPE_DRAW:
                if (!d3d12_command_list_begin_render_pass(list))
                {
                    WARN("Failed to begin render pass, ignoring draw.\n");
                    break;
                }

                VK_CALL(vkCmdDrawIndirect(list->vk_command_buffer, arg_impl->u.vk_buffer,
                        arg_buffer_offset, max_command_count, signature_desc->ByteStride));
                break;

            case D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED:
                if (!d3d12_command_list_begin_render_pass(list))
                {
                    WARN("Failed to begin render pass, ignoring draw.\n");
                    break;
                }

                d3d12_command_list_check_index_buffer_strip_cut_value(list);

                VK_CALL(vkCmdDrawIndexedIndirect(list->vk_command_buffer, arg_impl->u.vk_buffer,
                        arg_buffer_offset, max_command_count, signature_desc->ByteStride));
                break;

            case D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH:
                if (max_command_count != 1)
                    FIXME("Ignoring command count %u.\n", max_command_count);

                if (list->state->vk_bind_point != VK_PIPELINE_BIND_POINT_COMPUTE)
                {
                    WARN("Pipeline state %p has bind point %#x, ignoring dispatch.\n",
                            list->state, list->state->vk_bind_point);
                    break;
                }

                d3d12_command_list_end_current_render_pass(list);

                d3d12_command_list_update_descriptors(list, VK_PIPELINE_BIND_POINT_COMPUTE);

                VK_CALL(vkCmdDispatchIndirect(list->vk_command_buffer,
                        arg_impl->u.vk_buffer, arg_buffer_offset));
                break;

            default:
                FIXME("Ignoring unhandled argument type %#x.\n", arg_desc->Type);
                break;
        }
    }
}

static const struct ID3D12GraphicsCommandListVtbl d3d12_command_list_vtbl =
{
    /* IUnknown methods */
    d3d12_command_list_QueryInterface,
    d3d12_command_list_AddRef,
    d3d12_command_list_Release,
    /* ID3D12Object methods */
    d3d12_command_list_GetPrivateData,
    d3d12_command_list_SetPrivateData,
    d3d12_command_list_SetPrivateDataInterface,
    d3d12_command_list_SetName,
    /* ID3D12DeviceChild methods */
    d3d12_command_list_GetDevice,
    /* ID3D12CommandList methods */
    d3d12_command_list_GetType,
    /* ID3D12GraphicsCommandList methods */
    d3d12_command_list_Close,
    d3d12_command_list_Reset,
    d3d12_command_list_ClearState,
    d3d12_command_list_DrawInstanced,
    d3d12_command_list_DrawIndexedInstanced,
    d3d12_command_list_Dispatch,
    d3d12_command_list_CopyBufferRegion,
    d3d12_command_list_CopyTextureRegion,
    d3d12_command_list_CopyResource,
    d3d12_command_list_CopyTiles,
    d3d12_command_list_ResolveSubresource,
    d3d12_command_list_IASetPrimitiveTopology,
    d3d12_command_list_RSSetViewports,
    d3d12_command_list_RSSetScissorRects,
    d3d12_command_list_OMSetBlendFactor,
    d3d12_command_list_OMSetStencilRef,
    d3d12_command_list_SetPipelineState,
    d3d12_command_list_ResourceBarrier,
    d3d12_command_list_ExecuteBundle,
    d3d12_command_list_SetDescriptorHeaps,
    d3d12_command_list_SetComputeRootSignature,
    d3d12_command_list_SetGraphicsRootSignature,
    d3d12_command_list_SetComputeRootDescriptorTable,
    d3d12_command_list_SetGraphicsRootDescriptorTable,
    d3d12_command_list_SetComputeRoot32BitConstant,
    d3d12_command_list_SetGraphicsRoot32BitConstant,
    d3d12_command_list_SetComputeRoot32BitConstants,
    d3d12_command_list_SetGraphicsRoot32BitConstants,
    d3d12_command_list_SetComputeRootConstantBufferView,
    d3d12_command_list_SetGraphicsRootConstantBufferView,
    d3d12_command_list_SetComputeRootShaderResourceView,
    d3d12_command_list_SetGraphicsRootShaderResourceView,
    d3d12_command_list_SetComputeRootUnorderedAccessView,
    d3d12_command_list_SetGraphicsRootUnorderedAccessView,
    d3d12_command_list_IASetIndexBuffer,
    d3d12_command_list_IASetVertexBuffers,
    d3d12_command_list_SOSetTargets,
    d3d12_command_list_OMSetRenderTargets,
    d3d12_command_list_ClearDepthStencilView,
    d3d12_command_list_ClearRenderTargetView,
    d3d12_command_list_ClearUnorderedAccessViewUint,
    d3d12_command_list_ClearUnorderedAccessViewFloat,
    d3d12_command_list_DiscardResource,
    d3d12_command_list_BeginQuery,
    d3d12_command_list_EndQuery,
    d3d12_command_list_ResolveQueryData,
    d3d12_command_list_SetPredication,
    d3d12_command_list_SetMarker,
    d3d12_command_list_BeginEvent,
    d3d12_command_list_EndEvent,
    d3d12_command_list_ExecuteIndirect,
};

static struct d3d12_command_list *unsafe_impl_from_ID3D12CommandList(ID3D12CommandList *iface)
{
    if (!iface)
        return NULL;
    assert(iface->lpVtbl == (struct ID3D12CommandListVtbl *)&d3d12_command_list_vtbl);
    return CONTAINING_RECORD(iface, struct d3d12_command_list, ID3D12GraphicsCommandList_iface);
}

static HRESULT d3d12_command_list_init(struct d3d12_command_list *list, struct d3d12_device *device,
        D3D12_COMMAND_LIST_TYPE type, struct d3d12_command_allocator *allocator,
        ID3D12PipelineState *initial_pipeline_state)
{
    HRESULT hr;

    list->ID3D12GraphicsCommandList_iface.lpVtbl = &d3d12_command_list_vtbl;
    list->refcount = 1;

    list->type = type;

    if (FAILED(hr = vkd3d_private_store_init(&list->private_store)))
        return hr;

    list->device = device;
    ID3D12Device_AddRef(&device->ID3D12Device_iface);

    list->allocator = allocator;

    if (SUCCEEDED(hr = d3d12_command_allocator_allocate_command_buffer(allocator, list)))
    {
        d3d12_command_list_reset_state(list, initial_pipeline_state);
    }
    else
    {
        vkd3d_private_store_destroy(&list->private_store);
        ID3D12Device_Release(&device->ID3D12Device_iface);
    }

    return hr;
}

HRESULT d3d12_command_list_create(struct d3d12_device *device,
        UINT node_mask, D3D12_COMMAND_LIST_TYPE type, ID3D12CommandAllocator *allocator_iface,
        ID3D12PipelineState *initial_pipeline_state, struct d3d12_command_list **list)
{
    struct d3d12_command_allocator *allocator;
    struct d3d12_command_list *object;
    HRESULT hr;

    if (!(allocator = unsafe_impl_from_ID3D12CommandAllocator(allocator_iface)))
    {
        WARN("Command allocator is NULL.\n");
        return E_INVALIDARG;
    }

    if (allocator->type != type)
    {
        WARN("Command list types do not match (allocator %#x, list %#x).\n",
                allocator->type, type);
        return E_INVALIDARG;
    }

    debug_ignored_node_mask(node_mask);

    if (!(object = vkd3d_malloc(sizeof(*object))))
        return E_OUTOFMEMORY;

    if (FAILED(hr = d3d12_command_list_init(object, device, type, allocator, initial_pipeline_state)))
    {
        vkd3d_free(object);
        return hr;
    }

    TRACE("Created command list %p.\n", object);

    *list = object;

    return S_OK;
}

/* ID3D12CommandQueue */
static inline struct d3d12_command_queue *impl_from_ID3D12CommandQueue(ID3D12CommandQueue *iface)
{
    return CONTAINING_RECORD(iface, struct d3d12_command_queue, ID3D12CommandQueue_iface);
}

static HRESULT STDMETHODCALLTYPE d3d12_command_queue_QueryInterface(ID3D12CommandQueue *iface,
        REFIID riid, void **object)
{
    TRACE("iface %p, riid %s, object %p.\n", iface, debugstr_guid(riid), object);

    if (IsEqualGUID(riid, &IID_ID3D12CommandQueue)
            || IsEqualGUID(riid, &IID_ID3D12Pageable)
            || IsEqualGUID(riid, &IID_ID3D12DeviceChild)
            || IsEqualGUID(riid, &IID_ID3D12Object)
            || IsEqualGUID(riid, &IID_IUnknown))
    {
        ID3D12CommandQueue_AddRef(iface);
        *object = iface;
        return S_OK;
    }

    WARN("%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid(riid));

    *object = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE d3d12_command_queue_AddRef(ID3D12CommandQueue *iface)
{
    struct d3d12_command_queue *command_queue = impl_from_ID3D12CommandQueue(iface);
    ULONG refcount = InterlockedIncrement(&command_queue->refcount);

    TRACE("%p increasing refcount to %u.\n", command_queue, refcount);

    return refcount;
}

static ULONG STDMETHODCALLTYPE d3d12_command_queue_Release(ID3D12CommandQueue *iface)
{
    struct d3d12_command_queue *command_queue = impl_from_ID3D12CommandQueue(iface);
    ULONG refcount = InterlockedDecrement(&command_queue->refcount);

    TRACE("%p decreasing refcount to %u.\n", command_queue, refcount);

    if (!refcount)
    {
        struct d3d12_device *device = command_queue->device;

        vkd3d_private_store_destroy(&command_queue->private_store);

        vkd3d_free(command_queue);

        ID3D12Device_Release(&device->ID3D12Device_iface);
    }

    return refcount;
}

static HRESULT STDMETHODCALLTYPE d3d12_command_queue_GetPrivateData(ID3D12CommandQueue *iface,
        REFGUID guid, UINT *data_size, void *data)
{
    struct d3d12_command_queue *command_queue = impl_from_ID3D12CommandQueue(iface);

    TRACE("iface %p, guid %s, data_size %p, data %p.\n", iface, debugstr_guid(guid), data_size, data);

    return vkd3d_get_private_data(&command_queue->private_store, guid, data_size, data);
}

static HRESULT STDMETHODCALLTYPE d3d12_command_queue_SetPrivateData(ID3D12CommandQueue *iface,
        REFGUID guid, UINT data_size, const void *data)
{
    struct d3d12_command_queue *command_queue = impl_from_ID3D12CommandQueue(iface);

    TRACE("iface %p, guid %s, data_size %u, data %p.\n", iface, debugstr_guid(guid), data_size, data);

    return vkd3d_set_private_data(&command_queue->private_store, guid, data_size, data);
}

static HRESULT STDMETHODCALLTYPE d3d12_command_queue_SetPrivateDataInterface(ID3D12CommandQueue *iface,
        REFGUID guid, const IUnknown *data)
{
    struct d3d12_command_queue *command_queue = impl_from_ID3D12CommandQueue(iface);

    TRACE("iface %p, guid %s, data %p.\n", iface, debugstr_guid(guid), data);

    return vkd3d_set_private_data_interface(&command_queue->private_store, guid, data);
}

static HRESULT STDMETHODCALLTYPE d3d12_command_queue_SetName(ID3D12CommandQueue *iface, const WCHAR *name)
{
    struct d3d12_command_queue *command_queue = impl_from_ID3D12CommandQueue(iface);

    FIXME("iface %p, name %s stub!\n", iface, debugstr_w(name, command_queue->device->wchar_size));

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE d3d12_command_queue_GetDevice(ID3D12CommandQueue *iface,
        REFIID riid, void **device)
{
    struct d3d12_command_queue *command_queue = impl_from_ID3D12CommandQueue(iface);

    TRACE("iface %p, riid %s, device %p.\n", iface, debugstr_guid(riid), device);

    return ID3D12Device_QueryInterface(&command_queue->device->ID3D12Device_iface, riid, device);
}

static void STDMETHODCALLTYPE d3d12_command_queue_UpdateTileMappings(ID3D12CommandQueue *iface,
        ID3D12Resource *resource, UINT region_count,
        const D3D12_TILED_RESOURCE_COORDINATE *region_start_coordinates,
        const D3D12_TILE_REGION_SIZE *region_sizes,
        UINT range_count,
        const D3D12_TILE_RANGE_FLAGS *range_flags,
        UINT *heap_range_offsets,
        UINT *range_tile_counts,
        D3D12_TILE_MAPPING_FLAGS flags)
{
    FIXME("iface %p, resource %p, region_count %u, region_start_coordinates %p, "
            "region_sizes %p, range_count %u, range_flags %p, heap_range_offsets %p, "
            "range_tile_counts %p, flags %#x stub!\n",
            iface, resource, region_count, region_start_coordinates, region_sizes, range_count,
            range_flags, heap_range_offsets, range_tile_counts, flags);
}

static void STDMETHODCALLTYPE d3d12_command_queue_CopyTileMappings(ID3D12CommandQueue *iface,
        ID3D12Resource *dst_resource,
        const D3D12_TILED_RESOURCE_COORDINATE *dst_region_start_coordinate,
        ID3D12Resource *src_resource,
        const D3D12_TILED_RESOURCE_COORDINATE *src_region_start_coordinate,
        const D3D12_TILE_REGION_SIZE *region_size,
        D3D12_TILE_MAPPING_FLAGS flags)
{
    FIXME("iface %p, dst_resource %p, dst_region_start_coordinate %p, "
            "src_resource %p, src_region_start_coordinate %p, region_size %p, flags %#x stub!\n",
            iface, dst_resource, dst_region_start_coordinate, src_resource,
            src_region_start_coordinate, region_size, flags);
}

static void STDMETHODCALLTYPE d3d12_command_queue_ExecuteCommandLists(ID3D12CommandQueue *iface,
        UINT command_list_count, ID3D12CommandList * const *command_lists)
{
    struct d3d12_command_queue *command_queue = impl_from_ID3D12CommandQueue(iface);
    const struct vkd3d_vk_device_procs *vk_procs;
    struct d3d12_command_list *cmd_list;
    struct VkSubmitInfo submit_desc;
    VkCommandBuffer *buffers;
    VkQueue vk_queue;
    unsigned int i;
    VkResult vr;

    TRACE("iface %p, command_list_count %u, command_lists %p.\n",
            iface, command_list_count, command_lists);

    vk_procs = &command_queue->device->vk_procs;

    if (!(buffers = vkd3d_calloc(command_list_count, sizeof(*buffers))))
    {
        ERR("Failed to allocate command buffer array.\n");
        return;
    }

    for (i = 0; i < command_list_count; ++i)
    {
        cmd_list = unsafe_impl_from_ID3D12CommandList(command_lists[i]);

        if (cmd_list->is_recording)
        {
            d3d12_device_mark_as_removed(command_queue->device, DXGI_ERROR_INVALID_CALL,
                    "Command list %p is in recording state.\n", command_lists[i]);
            vkd3d_free(buffers);
            return;
        }

        buffers[i] = cmd_list->vk_command_buffer;
    }

    submit_desc.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit_desc.pNext = NULL;
    submit_desc.waitSemaphoreCount = 0;
    submit_desc.pWaitSemaphores = NULL;
    submit_desc.pWaitDstStageMask = NULL;
    submit_desc.commandBufferCount = command_list_count;
    submit_desc.pCommandBuffers = buffers;
    submit_desc.signalSemaphoreCount = 0;
    submit_desc.pSignalSemaphores = NULL;

    if (!(vk_queue = vkd3d_queue_acquire(command_queue->vkd3d_queue)))
    {
        ERR("Failed to acquire queue %p.\n", command_queue->vkd3d_queue);
        vkd3d_free(buffers);
        return;
    }

    if ((vr = VK_CALL(vkQueueSubmit(vk_queue, 1, &submit_desc, VK_NULL_HANDLE))) < 0)
        ERR("Failed to submit queue(s), vr %d.\n", vr);

    vkd3d_queue_release(command_queue->vkd3d_queue);

    vkd3d_free(buffers);
}

static void STDMETHODCALLTYPE d3d12_command_queue_SetMarker(ID3D12CommandQueue *iface,
        UINT metadata, const void *data, UINT size)
{
    FIXME("iface %p, metadata %#x, data %p, size %u stub!\n",
            iface, metadata, data, size);
}

static void STDMETHODCALLTYPE d3d12_command_queue_BeginEvent(ID3D12CommandQueue *iface,
        UINT metadata, const void *data, UINT size)
{
    FIXME("iface %p, metatdata %#x, data %p, size %u stub!\n",
            iface, metadata, data, size);
}

static void STDMETHODCALLTYPE d3d12_command_queue_EndEvent(ID3D12CommandQueue *iface)
{
    FIXME("iface %p stub!\n", iface);
}

static HRESULT STDMETHODCALLTYPE d3d12_command_queue_Signal(ID3D12CommandQueue *iface,
        ID3D12Fence *fence, UINT64 value)
{
    struct d3d12_command_queue *command_queue = impl_from_ID3D12CommandQueue(iface);
    const struct vkd3d_vk_device_procs *vk_procs;
    VkFenceCreateInfo fence_info;
    struct d3d12_device *device;
    VkFence vk_fence;
    VkQueue vk_queue;
    VkResult vr;

    TRACE("iface %p, fence %p, value %#"PRIx64".\n", iface, fence, value);

    device = command_queue->device;
    vk_procs = &device->vk_procs;

    fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fence_info.pNext = NULL;
    fence_info.flags = 0;

    /* XXX: It may be a good idea to re-use Vulkan fences. */
    if ((vr = VK_CALL(vkCreateFence(device->vk_device, &fence_info, NULL, &vk_fence))) < 0)
    {
        WARN("Failed to create Vulkan fence, vr %d.\n", vr);
        return hresult_from_vk_result(vr);
    }

    if (!(vk_queue = vkd3d_queue_acquire(command_queue->vkd3d_queue)))
    {
        ERR("Failed to acquire queue %p.\n", command_queue->vkd3d_queue);
        return E_FAIL;
    }
    vr = VK_CALL(vkQueueSubmit(vk_queue, 0, NULL, vk_fence));
    vkd3d_queue_release(command_queue->vkd3d_queue);
    if (vr < 0)
    {
        WARN("Failed to submit fence, vr %d.\n", vr);
        VK_CALL(vkDestroyFence(device->vk_device, vk_fence, NULL));
        return hresult_from_vk_result(vr);
    }

    vr = VK_CALL(vkGetFenceStatus(device->vk_device, vk_fence));
    if (vr == VK_SUCCESS)
    {
        VK_CALL(vkDestroyFence(device->vk_device, vk_fence, NULL));
        return d3d12_fence_Signal(fence, value);
    }
    if (vr != VK_NOT_READY)
    {
        WARN("Failed to get fence status, vr %d.\n", vr);
        VK_CALL(vkDestroyFence(device->vk_device, vk_fence, NULL));
        return hresult_from_vk_result(vr);
    }

    return vkd3d_enqueue_gpu_fence(&device->fence_worker, vk_fence, fence, value);
}

static HRESULT STDMETHODCALLTYPE d3d12_command_queue_Wait(ID3D12CommandQueue *iface,
        ID3D12Fence *fence, UINT64 value)
{
    FIXME("iface %p, fence %p, value %#"PRIx64" stub!\n", iface, fence, value);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE d3d12_command_queue_GetTimestampFrequency(ID3D12CommandQueue *iface,
        UINT64 *frequency)
{
    struct d3d12_command_queue *command_queue = impl_from_ID3D12CommandQueue(iface);
    struct d3d12_device *device = command_queue->device;

    TRACE("iface %p, frequency %p.\n", iface, frequency);

    if (!command_queue->vkd3d_queue->timestamp_bits)
    {
        WARN("Timestamp queries not supported.\n");
        return E_FAIL;
    }

    *frequency = 1000000000 / device->vk_info.device_limits.timestampPeriod;

    return S_OK;
}

static HRESULT STDMETHODCALLTYPE d3d12_command_queue_GetClockCalibration(ID3D12CommandQueue *iface,
        UINT64 *gpu_timestamp, UINT64 *cpu_timestamp)
{
    FIXME("iface %p, gpu_timestamp %p, cpu_timestamp %p stub!\n",
            iface, gpu_timestamp, cpu_timestamp);

    return E_NOTIMPL;
}

static D3D12_COMMAND_QUEUE_DESC * STDMETHODCALLTYPE d3d12_command_queue_GetDesc(ID3D12CommandQueue *iface,
        D3D12_COMMAND_QUEUE_DESC *desc)
{
    struct d3d12_command_queue *command_queue = impl_from_ID3D12CommandQueue(iface);

    TRACE("iface %p, desc %p.\n", iface, desc);

    *desc = command_queue->desc;
    return desc;
}

static const struct ID3D12CommandQueueVtbl d3d12_command_queue_vtbl =
{
    /* IUnknown methods */
    d3d12_command_queue_QueryInterface,
    d3d12_command_queue_AddRef,
    d3d12_command_queue_Release,
    /* ID3D12Object methods */
    d3d12_command_queue_GetPrivateData,
    d3d12_command_queue_SetPrivateData,
    d3d12_command_queue_SetPrivateDataInterface,
    d3d12_command_queue_SetName,
    /* ID3D12DeviceChild methods */
    d3d12_command_queue_GetDevice,
    /* ID3D12CommandQueue methods */
    d3d12_command_queue_UpdateTileMappings,
    d3d12_command_queue_CopyTileMappings,
    d3d12_command_queue_ExecuteCommandLists,
    d3d12_command_queue_SetMarker,
    d3d12_command_queue_BeginEvent,
    d3d12_command_queue_EndEvent,
    d3d12_command_queue_Signal,
    d3d12_command_queue_Wait,
    d3d12_command_queue_GetTimestampFrequency,
    d3d12_command_queue_GetClockCalibration,
    d3d12_command_queue_GetDesc,
};

static HRESULT d3d12_command_queue_init(struct d3d12_command_queue *queue,
        struct d3d12_device *device, const D3D12_COMMAND_QUEUE_DESC *desc)
{
    HRESULT hr;

    queue->ID3D12CommandQueue_iface.lpVtbl = &d3d12_command_queue_vtbl;
    queue->refcount = 1;

    queue->desc = *desc;
    if (!queue->desc.NodeMask)
        queue->desc.NodeMask = 0x1;

    if (!(queue->vkd3d_queue = d3d12_device_get_vkd3d_queue(device, desc->Type)))
        return E_NOTIMPL;

    if (desc->Priority)
        FIXME("Ignoring priority %#x.\n", desc->Priority);
    if (desc->Flags)
        FIXME("Ignoring flags %#x.\n", desc->Flags);

    if (FAILED(hr = vkd3d_private_store_init(&queue->private_store)))
        return hr;

    queue->device = device;
    ID3D12Device_AddRef(&device->ID3D12Device_iface);

    return S_OK;
}

HRESULT d3d12_command_queue_create(struct d3d12_device *device,
        const D3D12_COMMAND_QUEUE_DESC *desc, struct d3d12_command_queue **queue)
{
    struct d3d12_command_queue *object;
    HRESULT hr;

    if (!(object = vkd3d_malloc(sizeof(*object))))
        return E_OUTOFMEMORY;

    if (FAILED(hr = d3d12_command_queue_init(object, device, desc)))
    {
        vkd3d_free(object);
        return hr;
    }

    TRACE("Created command queue %p.\n", object);

    *queue = object;

    return S_OK;
}

uint32_t vkd3d_get_vk_queue_family_index(ID3D12CommandQueue *queue)
{
    struct d3d12_command_queue *d3d12_queue = impl_from_ID3D12CommandQueue(queue);

    return d3d12_queue->vkd3d_queue->vk_family_index;
}

VkQueue vkd3d_acquire_vk_queue(ID3D12CommandQueue *queue)
{
    struct d3d12_command_queue *d3d12_queue = impl_from_ID3D12CommandQueue(queue);

    return vkd3d_queue_acquire(d3d12_queue->vkd3d_queue);
}

void vkd3d_release_vk_queue(ID3D12CommandQueue *queue)
{
    struct d3d12_command_queue *d3d12_queue = impl_from_ID3D12CommandQueue(queue);

    return vkd3d_queue_release(d3d12_queue->vkd3d_queue);
}

/* ID3D12CommandSignature */
static inline struct d3d12_command_signature *impl_from_ID3D12CommandSignature(ID3D12CommandSignature *iface)
{
    return CONTAINING_RECORD(iface, struct d3d12_command_signature, ID3D12CommandSignature_iface);
}

static HRESULT STDMETHODCALLTYPE d3d12_command_signature_QueryInterface(ID3D12CommandSignature *iface,
        REFIID iid, void **out)
{
    TRACE("iface %p, iid %s, out %p.\n", iface, debugstr_guid(iid), out);

    if (IsEqualGUID(iid, &IID_ID3D12CommandSignature)
            || IsEqualGUID(iid, &IID_ID3D12Pageable)
            || IsEqualGUID(iid, &IID_ID3D12DeviceChild)
            || IsEqualGUID(iid, &IID_ID3D12Object)
            || IsEqualGUID(iid, &IID_IUnknown))
    {
        ID3D12CommandSignature_AddRef(iface);
        *out = iface;
        return S_OK;
    }

    WARN("%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid(iid));

    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE d3d12_command_signature_AddRef(ID3D12CommandSignature *iface)
{
    struct d3d12_command_signature *signature = impl_from_ID3D12CommandSignature(iface);
    ULONG refcount = InterlockedIncrement(&signature->refcount);

    TRACE("%p increasing refcount to %u.\n", signature, refcount);

    return refcount;
}

static ULONG STDMETHODCALLTYPE d3d12_command_signature_Release(ID3D12CommandSignature *iface)
{
    struct d3d12_command_signature *signature = impl_from_ID3D12CommandSignature(iface);
    ULONG refcount = InterlockedDecrement(&signature->refcount);

    TRACE("%p decreasing refcount to %u.\n", signature, refcount);

    if (!refcount)
    {
        struct d3d12_device *device = signature->device;

        vkd3d_private_store_destroy(&signature->private_store);

        vkd3d_free((void *)signature->desc.pArgumentDescs);
        vkd3d_free(signature);

        ID3D12Device_Release(&device->ID3D12Device_iface);
    }

    return refcount;
}

static HRESULT STDMETHODCALLTYPE d3d12_command_signature_GetPrivateData(ID3D12CommandSignature *iface,
        REFGUID guid, UINT *data_size, void *data)
{
    struct d3d12_command_signature *signature = impl_from_ID3D12CommandSignature(iface);

    TRACE("iface %p, guid %s, data_size %p, data %p.\n", iface, debugstr_guid(guid), data_size, data);

    return vkd3d_get_private_data(&signature->private_store, guid, data_size, data);
}

static HRESULT STDMETHODCALLTYPE d3d12_command_signature_SetPrivateData(ID3D12CommandSignature *iface,
        REFGUID guid, UINT data_size, const void *data)
{
    struct d3d12_command_signature *signature = impl_from_ID3D12CommandSignature(iface);

    TRACE("iface %p, guid %s, data_size %u, data %p.\n", iface, debugstr_guid(guid), data_size, data);

    return vkd3d_set_private_data(&signature->private_store, guid, data_size, data);
}

static HRESULT STDMETHODCALLTYPE d3d12_command_signature_SetPrivateDataInterface(ID3D12CommandSignature *iface,
        REFGUID guid, const IUnknown *data)
{
    struct d3d12_command_signature *signature = impl_from_ID3D12CommandSignature(iface);

    TRACE("iface %p, guid %s, data %p.\n", iface, debugstr_guid(guid), data);

    return vkd3d_set_private_data_interface(&signature->private_store, guid, data);
}

static HRESULT STDMETHODCALLTYPE d3d12_command_signature_SetName(ID3D12CommandSignature *iface, const WCHAR *name)
{
    struct d3d12_command_signature *signature = impl_from_ID3D12CommandSignature(iface);

    TRACE("iface %p, name %s.\n", iface, debugstr_w(name, signature->device->wchar_size));

    return name ? S_OK : E_INVALIDARG;
}

static HRESULT STDMETHODCALLTYPE d3d12_command_signature_GetDevice(ID3D12CommandSignature *iface,
        REFIID iid, void **device)
{
    struct d3d12_command_signature *signature = impl_from_ID3D12CommandSignature(iface);

    TRACE("iface %p, iid %s, device %p.\n", iface, debugstr_guid(iid), device);

    return ID3D12Device_QueryInterface(&signature->device->ID3D12Device_iface, iid, device);
}

static const struct ID3D12CommandSignatureVtbl d3d12_command_signature_vtbl =
{
    /* IUnknown methods */
    d3d12_command_signature_QueryInterface,
    d3d12_command_signature_AddRef,
    d3d12_command_signature_Release,
    /* ID3D12Object methods */
    d3d12_command_signature_GetPrivateData,
    d3d12_command_signature_SetPrivateData,
    d3d12_command_signature_SetPrivateDataInterface,
    d3d12_command_signature_SetName,
    /* ID3D12DeviceChild methods */
    d3d12_command_signature_GetDevice,
};

struct d3d12_command_signature *unsafe_impl_from_ID3D12CommandSignature(ID3D12CommandSignature *iface)
{
    if (!iface)
        return NULL;
    assert(iface->lpVtbl == &d3d12_command_signature_vtbl);
    return CONTAINING_RECORD(iface, struct d3d12_command_signature, ID3D12CommandSignature_iface);
}

HRESULT d3d12_command_signature_create(struct d3d12_device *device, const D3D12_COMMAND_SIGNATURE_DESC *desc,
        struct d3d12_command_signature **signature)
{
    struct d3d12_command_signature *object;
    unsigned int i;
    HRESULT hr;

    for (i = 0; i < desc->NumArgumentDescs; ++i)
    {
        const D3D12_INDIRECT_ARGUMENT_DESC *argument_desc = &desc->pArgumentDescs[i];
        switch (argument_desc->Type)
        {
            case D3D12_INDIRECT_ARGUMENT_TYPE_DRAW:
            case D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED:
            case D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH:
                if (i != desc->NumArgumentDescs - 1)
                {
                    WARN("Draw/dispatch must be the last element of a command signature.\n");
                    return E_INVALIDARG;
                }
                break;
            default:
                break;
        }
    }

    if (!(object = vkd3d_malloc(sizeof(*object))))
        return E_OUTOFMEMORY;

    object->ID3D12CommandSignature_iface.lpVtbl = &d3d12_command_signature_vtbl;
    object->refcount = 1;

    object->desc = *desc;
    if (!(object->desc.pArgumentDescs = vkd3d_calloc(desc->NumArgumentDescs, sizeof(*desc->pArgumentDescs))))
    {
        vkd3d_free(object);
        return E_OUTOFMEMORY;
    }
    memcpy((void *)object->desc.pArgumentDescs, desc->pArgumentDescs,
            desc->NumArgumentDescs * sizeof(*desc->pArgumentDescs));

    if (FAILED(hr = vkd3d_private_store_init(&object->private_store)))
    {
        vkd3d_free((void *)object->desc.pArgumentDescs);
        vkd3d_free(object);
        return hr;
    }

    object->device = device;
    ID3D12Device_AddRef(&device->ID3D12Device_iface);

    TRACE("Created command signature %p.\n", object);

    *signature = object;

    return S_OK;
}
