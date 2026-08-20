#define _GNU_SOURCE
#include "vfio.h"
#include "model.h"
#include "spec.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/vfio.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#define MOCK_MAX_FDS 32
#define MOCK_GROUP_ID 0
#define MOCK_BAR0_REGION_OFFSET 0x10000000000ull
#define MOCK_CONFIG_REGION_OFFSET 0x20000000000ull

typedef enum {
    MOCK_OBJECT_NONE = 0,
    MOCK_OBJECT_CONTAINER,
    MOCK_OBJECT_GROUP,
    MOCK_OBJECT_DEVICE
} mock_object_kind_t;

struct lt_mock_state {
    pthread_mutex_t lock;
    nvme_model_t   *model;
};

typedef struct {
    int                 fd;
    mock_object_kind_t  kind;
    int                 container_fd;
    int                 iommu_set;
    lt_mock_handle_t   *owner;
} mock_object_t;

/* Every open fd across every armed instance lives here so that ioctl,
 * close, mmap, pread and pwrite can find their owning instance from the
 * fd alone, exactly as the kernel would route by file descriptor. Only
 * mock_open() needs an extra hint, since it is handed a path rather than
 * an fd; it reads that hint from arming_instance below. */
static struct {
    pthread_mutex_t lock;
    mock_object_t   objects[MOCK_MAX_FDS];
} fd_table = { PTHREAD_MUTEX_INITIALIZER, { { 0, MOCK_OBJECT_NONE, 0, 0, NULL } } };

/* Bound for the duration of lt_mock_arm() through the synchronous open
 * sequence that immediately follows it in nvme_dev_open(), so mock_open()
 * can tell which instance a freshly created fd belongs to. Thread local so
 * that concurrent nvme_dev_open() calls on different threads each attach
 * their own instance without racing this hint. */
static __thread lt_mock_handle_t *arming_instance = NULL;

void lt_mock_config_defaults(lt_mock_config_t *cfg)
{
    cfg->image_path = "mock.img";
    cfg->capacity_bytes = 64ull * 1024ull * 1024ull;
    cfg->lba_bytes = 512;
    cfg->max_namespaces = 4;
    cfg->queue_latency_us = 0;
    cfg->serial = "DEADBEEF";
    cfg->model = "lantern mock nvme";
}

lt_mock_handle_t *lt_mock_arm(const lt_mock_config_t *cfg, char *errbuf, size_t errlen)
{
    lt_mock_handle_t *handle = calloc(1, sizeof(*handle));
    lt_mock_config_t local_cfg;

    if (!handle) {
        snprintf(errbuf, errlen, "Out of memory");
        errno = ENOMEM;
        return NULL;
    }
    pthread_mutex_init(&handle->lock, NULL);
    local_cfg = *cfg;
    handle->model = nvme_model_create(&local_cfg, errbuf, errlen);
    if (!handle->model) {
        pthread_mutex_destroy(&handle->lock);
        free(handle);
        errno = EIO;
        return NULL;
    }
    arming_instance = handle;
    return handle;
}

void lt_mock_disarm(lt_mock_handle_t *handle)
{
    if (!handle)
        return;
    if (arming_instance == handle)
        arming_instance = NULL;
    nvme_model_destroy(handle->model);
    pthread_mutex_destroy(&handle->lock);
    free(handle);
}

static mock_object_t *mock_lookup(int fd)
{
    int i;

    pthread_mutex_lock(&fd_table.lock);
    for (i = 0; i < MOCK_MAX_FDS; i++) {
        if (fd_table.objects[i].kind != MOCK_OBJECT_NONE && fd_table.objects[i].fd == fd) {
            pthread_mutex_unlock(&fd_table.lock);
            return &fd_table.objects[i];
        }
    }
    pthread_mutex_unlock(&fd_table.lock);
    return NULL;
}

static int mock_track(int fd, mock_object_kind_t kind, lt_mock_handle_t *owner)
{
    int i;

    pthread_mutex_lock(&fd_table.lock);
    for (i = 0; i < MOCK_MAX_FDS; i++) {
        if (fd_table.objects[i].kind == MOCK_OBJECT_NONE) {
            fd_table.objects[i].fd = fd;
            fd_table.objects[i].kind = kind;
            fd_table.objects[i].container_fd = -1;
            fd_table.objects[i].iommu_set = 0;
            fd_table.objects[i].owner = owner;
            pthread_mutex_unlock(&fd_table.lock);
            return 0;
        }
    }
    pthread_mutex_unlock(&fd_table.lock);
    return -1;
}

static int mock_new_fd(void)
{
    return (int)syscall(SYS_memfd_create, "lantern-vfio", 0);
}

static int mock_open(const char *path, int flags)
{
    int fd;
    mock_object_kind_t kind;
    lt_mock_handle_t *owner;
    char group_path[64];

    (void)flags;
    snprintf(group_path, sizeof(group_path), "/dev/vfio/%d", MOCK_GROUP_ID);

    if (strcmp(path, "/dev/vfio/vfio") == 0)
        kind = MOCK_OBJECT_CONTAINER;
    else if (strcmp(path, group_path) == 0)
        kind = MOCK_OBJECT_GROUP;
    else {
        errno = ENOENT;
        return -1;
    }

    owner = arming_instance;
    if (!owner) {
        errno = ENODEV;
        return -1;
    }
    fd = mock_new_fd();
    if (fd < 0)
        return -1;
    if (mock_track(fd, kind, owner) != 0) {
        close(fd);
        errno = EMFILE;
        return -1;
    }
    return fd;
}

static int mock_close(int fd)
{
    mock_object_t *obj = mock_lookup(fd);

    if (obj)
        obj->kind = MOCK_OBJECT_NONE;
    return close(fd);
}

static int mock_ioctl_container(mock_object_t *obj, unsigned long request, void *arg)
{
    switch (request) {
    case VFIO_GET_API_VERSION:
        return VFIO_API_VERSION;

    case VFIO_CHECK_EXTENSION: {
        unsigned long ext = (unsigned long)(uintptr_t)arg;
        return ext == VFIO_TYPE1_IOMMU ? 1 : 0;
    }

    case VFIO_SET_IOMMU: {
        unsigned long type = (unsigned long)(uintptr_t)arg;
        if (type != VFIO_TYPE1_IOMMU) {
            errno = EINVAL;
            return -1;
        }
        obj->iommu_set = 1;
        return 0;
    }

    case VFIO_IOMMU_GET_INFO: {
        struct vfio_iommu_type1_info *info = arg;
        if (info->argsz < sizeof(*info)) {
            errno = EINVAL;
            return -1;
        }
        info->flags = VFIO_IOMMU_INFO_PGSIZES;
        info->iova_pgsizes = NVME_PAGE_SIZE | (2ull << 20);
        info->argsz = sizeof(*info);
        return 0;
    }

    case VFIO_IOMMU_MAP_DMA: {
        struct vfio_iommu_type1_dma_map *map = arg;
        if (map->argsz < sizeof(*map) || !obj->iommu_set) {
            errno = EINVAL;
            return -1;
        }
        if ((map->iova & NVME_PAGE_MASK) || (map->size & NVME_PAGE_MASK) ||
            (map->vaddr & NVME_PAGE_MASK)) {
            errno = EINVAL;
            return -1;
        }
        return nvme_model_dma_map(obj->owner->model, map->iova, map->vaddr, map->size);
    }

    case VFIO_IOMMU_UNMAP_DMA: {
        struct vfio_iommu_type1_dma_unmap *unmap = arg;
        if (unmap->argsz < sizeof(*unmap) || !obj->iommu_set) {
            errno = EINVAL;
            return -1;
        }
        if (nvme_model_dma_unmap(obj->owner->model, unmap->iova, unmap->size) != 0)
            return -1;
        return 0;
    }

    default:
        errno = ENOTTY;
        return -1;
    }
}

static int mock_ioctl_group(mock_object_t *obj, unsigned long request, void *arg)
{
    switch (request) {
    case VFIO_GROUP_GET_STATUS: {
        struct vfio_group_status *status = arg;
        if (status->argsz < sizeof(*status)) {
            errno = EINVAL;
            return -1;
        }
        status->flags = VFIO_GROUP_FLAGS_VIABLE;
        if (obj->container_fd >= 0)
            status->flags |= VFIO_GROUP_FLAGS_CONTAINER_SET;
        return 0;
    }

    case VFIO_GROUP_SET_CONTAINER: {
        int *container = arg;
        mock_object_t *cobj = mock_lookup(*container);
        if (!cobj || cobj->kind != MOCK_OBJECT_CONTAINER) {
            errno = EINVAL;
            return -1;
        }
        obj->container_fd = *container;
        return 0;
    }

    case VFIO_GROUP_UNSET_CONTAINER:
        obj->container_fd = -1;
        return 0;

    case VFIO_GROUP_GET_DEVICE_FD: {
        const char *name = arg;
        int fd;
        if (obj->container_fd < 0) {
            errno = EINVAL;
            return -1;
        }
        if (!name || strlen(name) != 12) {
            errno = ENODEV;
            return -1;
        }
        fd = mock_new_fd();
        if (fd < 0)
            return -1;
        if (mock_track(fd, MOCK_OBJECT_DEVICE, obj->owner) != 0) {
            close(fd);
            errno = EMFILE;
            return -1;
        }
        return fd;
    }

    default:
        errno = ENOTTY;
        return -1;
    }
}

static int mock_ioctl_device(mock_object_t *obj, unsigned long request, void *arg)
{
    switch (request) {
    case VFIO_DEVICE_GET_INFO: {
        struct vfio_device_info *info = arg;
        if (info->argsz < sizeof(*info)) {
            errno = EINVAL;
            return -1;
        }
        info->flags = VFIO_DEVICE_FLAGS_PCI | VFIO_DEVICE_FLAGS_RESET;
        info->num_regions = VFIO_PCI_NUM_REGIONS;
        info->num_irqs = VFIO_PCI_NUM_IRQS;
        return 0;
    }

    case VFIO_DEVICE_GET_REGION_INFO: {
        struct vfio_region_info *info = arg;
        if (info->argsz < sizeof(*info)) {
            errno = EINVAL;
            return -1;
        }
        info->cap_offset = 0;
        if (info->index == VFIO_PCI_BAR0_REGION_INDEX) {
            info->flags = VFIO_REGION_INFO_FLAG_READ | VFIO_REGION_INFO_FLAG_WRITE |
                          VFIO_REGION_INFO_FLAG_MMAP;
            info->size = nvme_model_bar_size(obj->owner->model);
            info->offset = MOCK_BAR0_REGION_OFFSET;
            return 0;
        }
        if (info->index == VFIO_PCI_CONFIG_REGION_INDEX) {
            info->flags = VFIO_REGION_INFO_FLAG_READ | VFIO_REGION_INFO_FLAG_WRITE;
            info->size = nvme_model_config_size(obj->owner->model);
            info->offset = MOCK_CONFIG_REGION_OFFSET;
            return 0;
        }
        if (info->index >= VFIO_PCI_NUM_REGIONS) {
            errno = EINVAL;
            return -1;
        }
        info->flags = 0;
        info->size = 0;
        info->offset = 0;
        return 0;
    }

    case VFIO_DEVICE_GET_IRQ_INFO: {
        struct vfio_irq_info *info = arg;
        if (info->argsz < sizeof(*info)) {
            errno = EINVAL;
            return -1;
        }
        if (info->index != VFIO_PCI_MSIX_IRQ_INDEX) {
            info->flags = 0;
            info->count = 0;
            return 0;
        }
        info->flags = VFIO_IRQ_INFO_EVENTFD;
        info->count = 32;
        return 0;
    }

    case VFIO_DEVICE_SET_IRQS: {
        struct vfio_irq_set *set = arg;
        if (set->argsz < sizeof(*set)) {
            errno = EINVAL;
            return -1;
        }
        if (set->index != VFIO_PCI_MSIX_IRQ_INDEX) {
            errno = EINVAL;
            return -1;
        }
        if (set->flags & VFIO_IRQ_SET_DATA_NONE) {
            return nvme_model_set_msix(obj->owner->model, set->start, set->count, NULL);
        }
        if (set->flags & VFIO_IRQ_SET_DATA_EVENTFD) {
            const int32_t *fds = (const int32_t *)set->data;
            return nvme_model_set_msix(obj->owner->model, set->start, set->count, fds);
        }
        errno = EINVAL;
        return -1;
    }

    case VFIO_DEVICE_RESET:
        nvme_model_device_reset(obj->owner->model);
        return 0;

    default:
        errno = ENOTTY;
        return -1;
    }
}

static int mock_ioctl(int fd, unsigned long request, void *arg)
{
    mock_object_t *obj = mock_lookup(fd);
    lt_mock_handle_t *owner;
    int rc;

    if (!obj) {
        errno = EBADF;
        return -1;
    }
    owner = obj->owner;
    pthread_mutex_lock(&owner->lock);
    switch (obj->kind) {
    case MOCK_OBJECT_CONTAINER:
        rc = mock_ioctl_container(obj, request, arg);
        break;
    case MOCK_OBJECT_GROUP:
        rc = mock_ioctl_group(obj, request, arg);
        break;
    case MOCK_OBJECT_DEVICE:
        rc = mock_ioctl_device(obj, request, arg);
        break;
    default:
        errno = EBADF;
        rc = -1;
        break;
    }
    pthread_mutex_unlock(&owner->lock);
    return rc;
}

static void *mock_mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset)
{
    mock_object_t *obj;
    void *bar;

    (void)addr;
    (void)prot;
    (void)flags;

    obj = mock_lookup(fd);
    if (!obj || obj->kind != MOCK_OBJECT_DEVICE) {
        errno = EACCES;
        return MAP_FAILED;
    }
    pthread_mutex_lock(&obj->owner->lock);
    if ((uint64_t)offset != MOCK_BAR0_REGION_OFFSET ||
        length > nvme_model_bar_size(obj->owner->model)) {
        pthread_mutex_unlock(&obj->owner->lock);
        errno = EINVAL;
        return MAP_FAILED;
    }
    bar = nvme_model_bar(obj->owner->model);
    pthread_mutex_unlock(&obj->owner->lock);
    return bar;
}

static int mock_munmap(void *addr, size_t length)
{
    (void)addr;
    (void)length;
    return 0;
}

static ssize_t mock_pread(int fd, void *buf, size_t count, off_t offset)
{
    mock_object_t *obj;
    uint64_t region_offset;

    obj = mock_lookup(fd);
    if (!obj || obj->kind != MOCK_OBJECT_DEVICE) {
        errno = EBADF;
        return -1;
    }
    pthread_mutex_lock(&obj->owner->lock);
    region_offset = (uint64_t)offset - MOCK_CONFIG_REGION_OFFSET;
    if ((uint64_t)offset < MOCK_CONFIG_REGION_OFFSET ||
        region_offset + count > nvme_model_config_size(obj->owner->model)) {
        pthread_mutex_unlock(&obj->owner->lock);
        errno = EINVAL;
        return -1;
    }
    memcpy(buf, nvme_model_config_space(obj->owner->model) + region_offset, count);
    pthread_mutex_unlock(&obj->owner->lock);
    return (ssize_t)count;
}

static ssize_t mock_pwrite(int fd, const void *buf, size_t count, off_t offset)
{
    mock_object_t *obj;
    uint64_t region_offset;

    obj = mock_lookup(fd);
    if (!obj || obj->kind != MOCK_OBJECT_DEVICE) {
        errno = EBADF;
        return -1;
    }
    pthread_mutex_lock(&obj->owner->lock);
    region_offset = (uint64_t)offset - MOCK_CONFIG_REGION_OFFSET;
    if ((uint64_t)offset < MOCK_CONFIG_REGION_OFFSET ||
        region_offset + count > nvme_model_config_size(obj->owner->model)) {
        pthread_mutex_unlock(&obj->owner->lock);
        errno = EINVAL;
        return -1;
    }
    memcpy(nvme_model_config_space(obj->owner->model) + region_offset, buf, count);
    pthread_mutex_unlock(&obj->owner->lock);
    return (ssize_t)count;
}

static int mock_iommu_group_of(const char *bdf)
{
    if (!bdf || strlen(bdf) != 12) {
        errno = ENODEV;
        return -1;
    }
    return MOCK_GROUP_ID;
}

static const struct lt_syscall_ops mock_ops = {
    .name = "mock",
    .open = mock_open,
    .close = mock_close,
    .ioctl = mock_ioctl,
    .mmap = mock_mmap,
    .munmap = mock_munmap,
    .pread = mock_pread,
    .pwrite = mock_pwrite,
    .iommu_group_of = mock_iommu_group_of,
};

const struct lt_syscall_ops *lt_syscalls_mock(void)
{
    return &mock_ops;
}
