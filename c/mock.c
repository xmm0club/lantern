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

typedef struct {
    int                 fd;
    mock_object_kind_t  kind;
    int                 container_fd;
    int                 iommu_set;
} mock_object_t;

static struct {
    pthread_mutex_t lock;
    int             armed;
    lt_mock_config_t config;
    nvme_model_t   *model;
    mock_object_t   objects[MOCK_MAX_FDS];
    char            error[256];
} mock_state = { PTHREAD_MUTEX_INITIALIZER, 0, { NULL, 0, 0, 0, 0, NULL, NULL }, NULL, { { 0, 0, 0, 0 } }, { 0 } };

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

const char *lt_mock_last_error(void)
{
    return mock_state.error;
}

int lt_mock_arm(const lt_mock_config_t *cfg)
{
    pthread_mutex_lock(&mock_state.lock);
    if (mock_state.armed) {
        pthread_mutex_unlock(&mock_state.lock);
        errno = EBUSY;
        return -1;
    }
    mock_state.config = *cfg;
    mock_state.model = nvme_model_create(&mock_state.config, mock_state.error, sizeof(mock_state.error));
    if (!mock_state.model) {
        pthread_mutex_unlock(&mock_state.lock);
        errno = EIO;
        return -1;
    }
    memset(mock_state.objects, 0, sizeof(mock_state.objects));
    mock_state.armed = 1;
    pthread_mutex_unlock(&mock_state.lock);
    return 0;
}

void lt_mock_disarm(void)
{
    pthread_mutex_lock(&mock_state.lock);
    if (mock_state.model) {
        nvme_model_destroy(mock_state.model);
        mock_state.model = NULL;
    }
    mock_state.armed = 0;
    pthread_mutex_unlock(&mock_state.lock);
}

static mock_object_t *mock_lookup(int fd)
{
    int i;

    for (i = 0; i < MOCK_MAX_FDS; i++)
        if (mock_state.objects[i].kind != MOCK_OBJECT_NONE && mock_state.objects[i].fd == fd)
            return &mock_state.objects[i];
    return NULL;
}

static int mock_track(int fd, mock_object_kind_t kind)
{
    int i;

    for (i = 0; i < MOCK_MAX_FDS; i++) {
        if (mock_state.objects[i].kind == MOCK_OBJECT_NONE) {
            mock_state.objects[i].fd = fd;
            mock_state.objects[i].kind = kind;
            mock_state.objects[i].container_fd = -1;
            mock_state.objects[i].iommu_set = 0;
            return 0;
        }
    }
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

    pthread_mutex_lock(&mock_state.lock);
    if (!mock_state.armed) {
        pthread_mutex_unlock(&mock_state.lock);
        errno = ENODEV;
        return -1;
    }
    fd = mock_new_fd();
    if (fd < 0) {
        pthread_mutex_unlock(&mock_state.lock);
        return -1;
    }
    if (mock_track(fd, kind) != 0) {
        close(fd);
        pthread_mutex_unlock(&mock_state.lock);
        errno = EMFILE;
        return -1;
    }
    pthread_mutex_unlock(&mock_state.lock);
    return fd;
}

static int mock_close(int fd)
{
    mock_object_t *obj;

    pthread_mutex_lock(&mock_state.lock);
    obj = mock_lookup(fd);
    if (obj)
        obj->kind = MOCK_OBJECT_NONE;
    pthread_mutex_unlock(&mock_state.lock);
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
        return nvme_model_dma_map(mock_state.model, map->iova, map->vaddr, map->size);
    }

    case VFIO_IOMMU_UNMAP_DMA: {
        struct vfio_iommu_type1_dma_unmap *unmap = arg;
        if (unmap->argsz < sizeof(*unmap) || !obj->iommu_set) {
            errno = EINVAL;
            return -1;
        }
        if (nvme_model_dma_unmap(mock_state.model, unmap->iova, unmap->size) != 0)
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
        if (mock_track(fd, MOCK_OBJECT_DEVICE) != 0) {
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
    (void)obj;

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
            info->size = nvme_model_bar_size(mock_state.model);
            info->offset = MOCK_BAR0_REGION_OFFSET;
            return 0;
        }
        if (info->index == VFIO_PCI_CONFIG_REGION_INDEX) {
            info->flags = VFIO_REGION_INFO_FLAG_READ | VFIO_REGION_INFO_FLAG_WRITE;
            info->size = nvme_model_config_size(mock_state.model);
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
            return nvme_model_set_msix(mock_state.model, set->start, set->count, NULL);
        }
        if (set->flags & VFIO_IRQ_SET_DATA_EVENTFD) {
            const int32_t *fds = (const int32_t *)set->data;
            return nvme_model_set_msix(mock_state.model, set->start, set->count, fds);
        }
        errno = EINVAL;
        return -1;
    }

    case VFIO_DEVICE_RESET:
        nvme_model_device_reset(mock_state.model);
        return 0;

    default:
        errno = ENOTTY;
        return -1;
    }
}

static int mock_ioctl(int fd, unsigned long request, void *arg)
{
    mock_object_t *obj;
    int rc;

    pthread_mutex_lock(&mock_state.lock);
    obj = mock_lookup(fd);
    if (!obj) {
        pthread_mutex_unlock(&mock_state.lock);
        errno = EBADF;
        return -1;
    }
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
    pthread_mutex_unlock(&mock_state.lock);
    return rc;
}

static void *mock_mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset)
{
    mock_object_t *obj;
    void *bar;

    (void)addr;
    (void)prot;
    (void)flags;

    pthread_mutex_lock(&mock_state.lock);
    obj = mock_lookup(fd);
    if (!obj || obj->kind != MOCK_OBJECT_DEVICE) {
        pthread_mutex_unlock(&mock_state.lock);
        errno = EACCES;
        return MAP_FAILED;
    }
    if ((uint64_t)offset != MOCK_BAR0_REGION_OFFSET ||
        length > nvme_model_bar_size(mock_state.model)) {
        pthread_mutex_unlock(&mock_state.lock);
        errno = EINVAL;
        return MAP_FAILED;
    }
    bar = nvme_model_bar(mock_state.model);
    pthread_mutex_unlock(&mock_state.lock);
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

    pthread_mutex_lock(&mock_state.lock);
    obj = mock_lookup(fd);
    if (!obj || obj->kind != MOCK_OBJECT_DEVICE) {
        pthread_mutex_unlock(&mock_state.lock);
        errno = EBADF;
        return -1;
    }
    region_offset = (uint64_t)offset - MOCK_CONFIG_REGION_OFFSET;
    if ((uint64_t)offset < MOCK_CONFIG_REGION_OFFSET ||
        region_offset + count > nvme_model_config_size(mock_state.model)) {
        pthread_mutex_unlock(&mock_state.lock);
        errno = EINVAL;
        return -1;
    }
    memcpy(buf, nvme_model_config_space(mock_state.model) + region_offset, count);
    pthread_mutex_unlock(&mock_state.lock);
    return (ssize_t)count;
}

static ssize_t mock_pwrite(int fd, const void *buf, size_t count, off_t offset)
{
    mock_object_t *obj;
    uint64_t region_offset;

    pthread_mutex_lock(&mock_state.lock);
    obj = mock_lookup(fd);
    if (!obj || obj->kind != MOCK_OBJECT_DEVICE) {
        pthread_mutex_unlock(&mock_state.lock);
        errno = EBADF;
        return -1;
    }
    region_offset = (uint64_t)offset - MOCK_CONFIG_REGION_OFFSET;
    if ((uint64_t)offset < MOCK_CONFIG_REGION_OFFSET ||
        region_offset + count > nvme_model_config_size(mock_state.model)) {
        pthread_mutex_unlock(&mock_state.lock);
        errno = EINVAL;
        return -1;
    }
    memcpy(nvme_model_config_space(mock_state.model) + region_offset, buf, count);
    pthread_mutex_unlock(&mock_state.lock);
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
