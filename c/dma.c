#define _GNU_SOURCE
#include "dma.h"
#include "spec.h"
#include "vfio.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/vfio.h>
#include <poll.h>
#include <sched.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define LANTERN_IOVA_BASE     0x100000000ull
#define LANTERN_MAX_DMA       128
#define LANTERN_MAX_VECTORS   32
#define LANTERN_ERROR_LEN     256
#define LANTERN_PRP_PER_PAGE  (NVME_PAGE_SIZE / (unsigned)sizeof(uint64_t))

struct nvme_dma {
    int      in_use;
    void    *addr;
    uint64_t iova;
    size_t   size;
};

typedef struct {
    int        active;
    uint16_t   qid;
    uint16_t   depth;
    uint16_t   sq_tail;
    uint16_t   cq_head;
    uint8_t    cq_phase;
    nvme_dma_t *sq_mem;
    nvme_dma_t *cq_mem;
    nvme_dma_t *prp_mem;
    uint32_t  *sq_doorbell;
    uint32_t  *cq_doorbell;
    uint64_t   submitted;
    uint64_t   completed;
} nvme_queue_t;

struct nvme_dev {
    const struct lt_syscall_ops *ops;
    char     bdf[16];
    int      backend;
    int      container_fd;
    int      group_fd;
    int      device_fd;
    uint8_t *bar;
    size_t   bar_size;
    uint64_t bar_region_offset;
    uint64_t config_region_offset;

    uint64_t cap;
    uint32_t doorbell_stride;
    uint32_t max_queue_entries;
    uint32_t timeout_ms;
    uint32_t page_size;
    uint32_t max_transfer_bytes;

    struct nvme_dma dma[LANTERN_MAX_DMA];
    nvme_queue_t queues[NVME_DMA_MAX_QUEUES];

    int      irq_fds[LANTERN_MAX_VECTORS];
    unsigned irq_count;

    char error[LANTERN_ERROR_LEN];
};

static void set_error(nvme_dev_t *dev, const char *fmt, ...)
{
    va_list ap;

    if (!dev)
        return;
    va_start(ap, fmt);
    vsnprintf(dev->error, sizeof(dev->error), fmt, ap);
    va_end(ap);
}

static uint64_t now_ns(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void relax(unsigned spins)
{
    if ((spins & 0x3ffu) == 0x3ffu)
        sched_yield();
    else
        __builtin_ia32_pause();
}

static uint32_t mmio_read32(const nvme_dev_t *dev, uint32_t offset)
{
    return __atomic_load_n((uint32_t *)(dev->bar + offset), __ATOMIC_ACQUIRE);
}

static void mmio_write32(nvme_dev_t *dev, uint32_t offset, uint32_t value)
{
    __atomic_store_n((uint32_t *)(dev->bar + offset), value, __ATOMIC_RELEASE);
}

static uint64_t mmio_read64(const nvme_dev_t *dev, uint32_t offset)
{
    uint64_t low = mmio_read32(dev, offset);
    uint64_t high = mmio_read32(dev, offset + 4);
    return low | (high << 32);
}

static void mmio_write64(nvme_dev_t *dev, uint32_t offset, uint64_t value)
{
    mmio_write32(dev, offset, (uint32_t)value);
    mmio_write32(dev, offset + 4, (uint32_t)(value >> 32));
}

static uint32_t *doorbell_ptr(nvme_dev_t *dev, uint16_t qid, int completion)
{
    uint32_t stride = 4u << dev->doorbell_stride;
    uint32_t offset = NVME_REG_DOORBELL_BASE + (2u * (uint32_t)qid + (completion ? 1u : 0u)) * stride;
    return (uint32_t *)(dev->bar + offset);
}

static size_t page_round_up(size_t value)
{
    return (value + NVME_PAGE_SIZE - 1u) & ~(size_t)(NVME_PAGE_SIZE - 1u);
}

static uint64_t iova_allocate(nvme_dev_t *dev, size_t size)
{
    uint64_t candidate = LANTERN_IOVA_BASE;
    int moved = 1;

    while (moved) {
        int i;
        moved = 0;
        for (i = 0; i < LANTERN_MAX_DMA; i++) {
            struct nvme_dma *region = &dev->dma[i];
            if (!region->in_use)
                continue;
            if (candidate < region->iova + region->size && region->iova < candidate + size) {
                candidate = region->iova + region->size;
                moved = 1;
            }
        }
    }
    return candidate;
}

int nvme_dma_alloc(nvme_dev_t *dev, size_t size, nvme_dma_t **out)
{
    struct vfio_iommu_type1_dma_map map;
    struct nvme_dma *region = NULL;
    void *addr;
    size_t rounded;
    int i;

    if (!dev || !out || size == 0)
        return NVME_DMA_ERR_INVALID;

    for (i = 0; i < LANTERN_MAX_DMA; i++) {
        if (!dev->dma[i].in_use) {
            region = &dev->dma[i];
            break;
        }
    }
    if (!region) {
        set_error(dev, "DMA region table exhausted");
        return NVME_DMA_ERR_NOMEM;
    }

    rounded = page_round_up(size);
    addr = mmap(NULL, rounded, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS | MAP_LOCKED, -1, 0);
    if (addr == MAP_FAILED)
        addr = mmap(NULL, rounded, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (addr == MAP_FAILED) {
        set_error(dev, "Anonymous mapping of %zu bytes failed: %s", rounded, strerror(errno));
        return NVME_DMA_ERR_NOMEM;
    }
    memset(addr, 0, rounded);

    region->in_use = 1;
    region->addr = addr;
    region->size = rounded;
    region->iova = iova_allocate(dev, rounded);

    memset(&map, 0, sizeof(map));
    map.argsz = sizeof(map);
    map.flags = VFIO_DMA_MAP_FLAG_READ | VFIO_DMA_MAP_FLAG_WRITE;
    map.vaddr = (uint64_t)(uintptr_t)addr;
    map.iova = region->iova;
    map.size = rounded;

    if (dev->ops->ioctl(dev->container_fd, VFIO_IOMMU_MAP_DMA, &map) != 0) {
        set_error(dev, "VFIO_IOMMU_MAP_DMA IOVA=0x%llx size=%zu failed: %s",
                  (unsigned long long)region->iova, rounded, strerror(errno));
        munmap(addr, rounded);
        region->in_use = 0;
        return NVME_DMA_ERR_BACKEND;
    }

    *out = region;
    return NVME_DMA_OK;
}

void nvme_dma_free(nvme_dev_t *dev, nvme_dma_t *buffer)
{
    struct vfio_iommu_type1_dma_unmap unmap;

    if (!dev || !buffer || !buffer->in_use)
        return;

    memset(&unmap, 0, sizeof(unmap));
    unmap.argsz = sizeof(unmap);
    unmap.iova = buffer->iova;
    unmap.size = buffer->size;
    dev->ops->ioctl(dev->container_fd, VFIO_IOMMU_UNMAP_DMA, &unmap);
    munmap(buffer->addr, buffer->size);
    buffer->in_use = 0;
    buffer->addr = NULL;
    buffer->size = 0;
    buffer->iova = 0;
}

uint64_t nvme_dma_iova(const nvme_dma_t *buffer)
{
    return buffer ? buffer->iova : 0;
}

size_t nvme_dma_size(const nvme_dma_t *buffer)
{
    return buffer ? buffer->size : 0;
}

int nvme_dma_write(nvme_dma_t *buffer, size_t offset, const void *src, size_t len)
{
    if (!buffer || !buffer->in_use || !src)
        return NVME_DMA_ERR_INVALID;
    if (offset + len > buffer->size)
        return NVME_DMA_ERR_RANGE;
    memcpy((uint8_t *)buffer->addr + offset, src, len);
    return NVME_DMA_OK;
}

int nvme_dma_read(const nvme_dma_t *buffer, size_t offset, void *dst, size_t len)
{
    if (!buffer || !buffer->in_use || !dst)
        return NVME_DMA_ERR_INVALID;
    if (offset + len > buffer->size)
        return NVME_DMA_ERR_RANGE;
    memcpy(dst, (const uint8_t *)buffer->addr + offset, len);
    return NVME_DMA_OK;
}

int nvme_dma_fill(nvme_dma_t *buffer, size_t offset, int byte, size_t len)
{
    if (!buffer || !buffer->in_use)
        return NVME_DMA_ERR_INVALID;
    if (offset + len > buffer->size)
        return NVME_DMA_ERR_RANGE;
    memset((uint8_t *)buffer->addr + offset, byte, len);
    return NVME_DMA_OK;
}

int nvme_dma_pattern(nvme_dma_t *buffer, size_t offset, size_t len, uint64_t seed)
{
    uint64_t state = seed ? seed : 0x9e3779b97f4a7c15ull;
    uint8_t *cursor;
    size_t i;

    if (!buffer || !buffer->in_use)
        return NVME_DMA_ERR_INVALID;
    if (offset + len > buffer->size)
        return NVME_DMA_ERR_RANGE;

    cursor = (uint8_t *)buffer->addr + offset;
    for (i = 0; i + 8 <= len; i += 8) {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        memcpy(cursor + i, &state, 8);
    }
    for (; i < len; i++)
        cursor[i] = (uint8_t)(state >> ((i & 7u) * 8u));
    return NVME_DMA_OK;
}

int nvme_dma_equal(const nvme_dma_t *a, const nvme_dma_t *b, size_t offset, size_t len)
{
    if (!a || !b || !a->in_use || !b->in_use)
        return NVME_DMA_ERR_INVALID;
    if (offset + len > a->size || offset + len > b->size)
        return NVME_DMA_ERR_RANGE;
    return memcmp((const uint8_t *)a->addr + offset, (const uint8_t *)b->addr + offset, len) == 0;
}

int nvme_dma_id(const nvme_dev_t *dev, const nvme_dma_t *buffer)
{
    ptrdiff_t index;

    if (!dev || !buffer)
        return -1;
    index = buffer - dev->dma;
    if (index < 0 || index >= LANTERN_MAX_DMA || !dev->dma[index].in_use)
        return -1;
    return (int)index;
}

nvme_dma_t *nvme_dma_handle(nvme_dev_t *dev, int id)
{
    if (!dev || id < 0 || id >= LANTERN_MAX_DMA || !dev->dma[id].in_use)
        return NULL;
    return &dev->dma[id];
}

void nvme_sqe_init(nvme_sqe_t *sqe, uint8_t opcode, uint16_t cid, uint32_t nsid)
{
    memset(sqe, 0, sizeof(*sqe));
    sqe->dw[0] = (uint32_t)opcode | ((uint32_t)cid << 16);
    sqe->dw[1] = nsid;
}

void nvme_sqe_set_prp(nvme_sqe_t *sqe, uint64_t prp1, uint64_t prp2)
{
    sqe->dw[6] = (uint32_t)prp1;
    sqe->dw[7] = (uint32_t)(prp1 >> 32);
    sqe->dw[8] = (uint32_t)prp2;
    sqe->dw[9] = (uint32_t)(prp2 >> 32);
}

void nvme_sqe_set_cdw(nvme_sqe_t *sqe, unsigned index, uint32_t value)
{
    if (index < 16)
        sqe->dw[index] = value;
}

uint32_t nvme_sqe_get_cdw(const nvme_sqe_t *sqe, unsigned index)
{
    return index < 16 ? sqe->dw[index] : 0;
}

uint16_t nvme_sqe_cid(const nvme_sqe_t *sqe)
{
    return (uint16_t)(sqe->dw[0] >> 16);
}

int nvme_prp_build(nvme_dev_t *dev, uint16_t qid, uint16_t cid, const nvme_dma_t *buffer,
                   size_t offset, size_t len, nvme_sqe_t *sqe)
{
    nvme_queue_t *queue;
    uint64_t base;
    uint64_t first;
    uint64_t remaining;
    uint64_t prp1;
    uint64_t prp2 = 0;

    if (!dev || !buffer || !sqe || qid >= NVME_DMA_MAX_QUEUES)
        return NVME_DMA_ERR_INVALID;
    queue = &dev->queues[qid];
    if (!queue->prp_mem || cid >= queue->depth)
        return NVME_DMA_ERR_INVALID;
    if (offset + len > buffer->size)
        return NVME_DMA_ERR_RANGE;
    if (len == 0 || len > dev->max_transfer_bytes)
        return NVME_DMA_ERR_RANGE;

    base = buffer->iova + offset;
    prp1 = base;
    first = NVME_PAGE_SIZE - (base & NVME_PAGE_MASK);
    if (first >= len) {
        nvme_sqe_set_prp(sqe, prp1, 0);
        return NVME_DMA_OK;
    }

    remaining = len - first;
    if (remaining <= NVME_PAGE_SIZE) {
        prp2 = (base + first) & ~NVME_PAGE_MASK;
        nvme_sqe_set_prp(sqe, prp1, prp2);
        return NVME_DMA_OK;
    }

    {
        uint64_t list_iova = queue->prp_mem->iova + (uint64_t)cid * NVME_PAGE_SIZE;
        uint64_t *list = (uint64_t *)((uint8_t *)queue->prp_mem->addr + (size_t)cid * NVME_PAGE_SIZE);
        uint64_t cursor = (base + first) & ~NVME_PAGE_MASK;
        unsigned index = 0;
        unsigned pages = (unsigned)((remaining + NVME_PAGE_SIZE - 1u) / NVME_PAGE_SIZE);

        if (pages > LANTERN_PRP_PER_PAGE) {
            set_error(dev, "Transfer of %zu bytes needs %u PRP entries", len, pages);
            return NVME_DMA_ERR_RANGE;
        }
        while (index < pages) {
            list[index++] = cursor;
            cursor += NVME_PAGE_SIZE;
        }
        prp2 = list_iova;
        nvme_sqe_set_prp(sqe, prp1, prp2);
        return NVME_DMA_OK;
    }
}

static int queue_submit(nvme_dev_t *dev, nvme_queue_t *queue, const nvme_sqe_t *sqe)
{
    uint8_t *slot;
    uint16_t next;

    if (!queue->active)
        return NVME_DMA_ERR_STATE;

    if (queue->submitted - queue->completed >= (uint64_t)queue->depth - 1u) {
        set_error(dev, "Queue %u full with %llu outstanding commands", queue->qid,
                  (unsigned long long)(queue->submitted - queue->completed));
        return NVME_DMA_ERR_QUEUE_FULL;
    }

    next = (uint16_t)((queue->sq_tail + 1u) % queue->depth);

    slot = (uint8_t *)queue->sq_mem->addr + (size_t)queue->sq_tail * NVME_SQE_BYTES;
    memcpy(slot, sqe, NVME_SQE_BYTES);
    __atomic_thread_fence(__ATOMIC_RELEASE);

    queue->sq_tail = next;
    queue->submitted++;
    __atomic_store_n(queue->sq_doorbell, queue->sq_tail, __ATOMIC_RELEASE);
    return NVME_DMA_OK;
}

static int queue_poll(nvme_dev_t *dev, nvme_queue_t *queue, nvme_cqe_t *out, int timeout_ms)
{
    uint64_t deadline = timeout_ms < 0 ? 0 : now_ns() + (uint64_t)timeout_ms * 1000000ull;
    unsigned spins = 0;

    if (!queue->active)
        return NVME_DMA_ERR_STATE;

    for (;;) {
        struct nvme_wire_cqe *slot =
            (struct nvme_wire_cqe *)((uint8_t *)queue->cq_mem->addr +
                                     (size_t)queue->cq_head * NVME_CQE_BYTES);
        uint16_t status = __atomic_load_n(&slot->status, __ATOMIC_ACQUIRE);

        if ((status & 1u) == queue->cq_phase) {
            out->dw0 = slot->dw0;
            out->sq_head = slot->sq_head;
            out->sq_id = slot->sq_id;
            out->cid = slot->cid;
            out->status = (uint16_t)(status >> 1);

            queue->cq_head = (uint16_t)((queue->cq_head + 1u) % queue->depth);
            if (queue->cq_head == 0)
                queue->cq_phase ^= 1u;
            queue->completed++;
            __atomic_store_n(queue->cq_doorbell, queue->cq_head, __ATOMIC_RELEASE);
            return 1;
        }

        if (mmio_read32(dev, NVME_REG_CSTS) & NVME_CSTS_CFS) {
            set_error(dev, "Controller fatal status while polling queue %u", queue->qid);
            return NVME_DMA_ERR_CONTROLLER;
        }
        if (timeout_ms == 0)
            return 0;
        if (timeout_ms > 0 && now_ns() >= deadline)
            return 0;
        relax(spins++);
    }
}

int nvme_admin_submit(nvme_dev_t *dev, const nvme_sqe_t *sqe)
{
    if (!dev || !sqe)
        return NVME_DMA_ERR_INVALID;
    return queue_submit(dev, &dev->queues[0], sqe);
}

int nvme_admin_poll(nvme_dev_t *dev, nvme_cqe_t *cqe, int timeout_ms)
{
    if (!dev || !cqe)
        return NVME_DMA_ERR_INVALID;
    return queue_poll(dev, &dev->queues[0], cqe, timeout_ms);
}

int nvme_io_submit(nvme_dev_t *dev, uint16_t qid, const nvme_sqe_t *sqe)
{
    if (!dev || !sqe || qid == 0 || qid >= NVME_DMA_MAX_QUEUES)
        return NVME_DMA_ERR_INVALID;
    return queue_submit(dev, &dev->queues[qid], sqe);
}

int nvme_io_poll(nvme_dev_t *dev, uint16_t qid, nvme_cqe_t *cqe, int timeout_ms)
{
    if (!dev || !cqe || qid == 0 || qid >= NVME_DMA_MAX_QUEUES)
        return NVME_DMA_ERR_INVALID;
    return queue_poll(dev, &dev->queues[qid], cqe, timeout_ms);
}

static int queue_alloc_memory(nvme_dev_t *dev, nvme_queue_t *queue, uint16_t qid, uint16_t depth)
{
    int rc;

    queue->qid = qid;
    queue->depth = depth;
    queue->sq_tail = 0;
    queue->cq_head = 0;
    queue->cq_phase = 1;
    queue->submitted = 0;
    queue->completed = 0;

    rc = nvme_dma_alloc(dev, (size_t)depth * NVME_SQE_BYTES, &queue->sq_mem);
    if (rc != NVME_DMA_OK)
        return rc;
    rc = nvme_dma_alloc(dev, (size_t)depth * NVME_CQE_BYTES, &queue->cq_mem);
    if (rc != NVME_DMA_OK) {
        nvme_dma_free(dev, queue->sq_mem);
        queue->sq_mem = NULL;
        return rc;
    }
    rc = nvme_dma_alloc(dev, (size_t)depth * NVME_PAGE_SIZE, &queue->prp_mem);
    if (rc != NVME_DMA_OK) {
        nvme_dma_free(dev, queue->sq_mem);
        nvme_dma_free(dev, queue->cq_mem);
        queue->sq_mem = NULL;
        queue->cq_mem = NULL;
        return rc;
    }
    queue->sq_doorbell = doorbell_ptr(dev, qid, 0);
    queue->cq_doorbell = doorbell_ptr(dev, qid, 1);
    return NVME_DMA_OK;
}

static void queue_release(nvme_dev_t *dev, nvme_queue_t *queue)
{
    if (queue->sq_mem)
        nvme_dma_free(dev, queue->sq_mem);
    if (queue->cq_mem)
        nvme_dma_free(dev, queue->cq_mem);
    if (queue->prp_mem)
        nvme_dma_free(dev, queue->prp_mem);
    memset(queue, 0, sizeof(*queue));
}

int nvme_io_queue_alloc(nvme_dev_t *dev, uint16_t qid, uint16_t depth)
{
    if (!dev || qid == 0 || qid >= NVME_DMA_MAX_QUEUES)
        return NVME_DMA_ERR_INVALID;
    if (depth < 2 || depth > dev->max_queue_entries)
        return NVME_DMA_ERR_INVALID;
    if (dev->queues[qid].sq_mem)
        return NVME_DMA_ERR_STATE;
    return queue_alloc_memory(dev, &dev->queues[qid], qid, depth);
}

int nvme_io_queue_addresses(const nvme_dev_t *dev, uint16_t qid, uint64_t *sq_iova, uint64_t *cq_iova)
{
    const nvme_queue_t *queue;

    if (!dev || qid == 0 || qid >= NVME_DMA_MAX_QUEUES || !sq_iova || !cq_iova)
        return NVME_DMA_ERR_INVALID;
    queue = &dev->queues[qid];
    if (!queue->sq_mem || !queue->cq_mem)
        return NVME_DMA_ERR_STATE;
    *sq_iova = queue->sq_mem->iova;
    *cq_iova = queue->cq_mem->iova;
    return NVME_DMA_OK;
}

int nvme_io_queue_activate(nvme_dev_t *dev, uint16_t qid)
{
    if (!dev || qid == 0 || qid >= NVME_DMA_MAX_QUEUES)
        return NVME_DMA_ERR_INVALID;
    if (!dev->queues[qid].sq_mem)
        return NVME_DMA_ERR_STATE;
    dev->queues[qid].active = 1;
    return NVME_DMA_OK;
}

int nvme_io_queue_free(nvme_dev_t *dev, uint16_t qid)
{
    if (!dev || qid == 0 || qid >= NVME_DMA_MAX_QUEUES)
        return NVME_DMA_ERR_INVALID;
    queue_release(dev, &dev->queues[qid]);
    return NVME_DMA_OK;
}

int nvme_irq_enable(nvme_dev_t *dev, unsigned vectors)
{
    struct vfio_irq_info info;
    struct vfio_irq_set *set;
    size_t argsz;
    unsigned i;
    int rc = NVME_DMA_OK;

    if (!dev || vectors == 0 || vectors > LANTERN_MAX_VECTORS)
        return NVME_DMA_ERR_INVALID;
    if (dev->irq_count != 0)
        return NVME_DMA_ERR_STATE;

    memset(&info, 0, sizeof(info));
    info.argsz = sizeof(info);
    info.index = VFIO_PCI_MSIX_IRQ_INDEX;
    if (dev->ops->ioctl(dev->device_fd, VFIO_DEVICE_GET_IRQ_INFO, &info) != 0) {
        set_error(dev, "VFIO_DEVICE_GET_IRQ_INFO failed: %s", strerror(errno));
        return NVME_DMA_ERR_BACKEND;
    }
    if (info.count < vectors) {
        set_error(dev, "Device offers %u MSI-X vectors, %u requested", info.count, vectors);
        return NVME_DMA_ERR_INVALID;
    }

    argsz = sizeof(struct vfio_irq_set) + vectors * sizeof(int32_t);
    set = calloc(1, argsz);
    if (!set)
        return NVME_DMA_ERR_NOMEM;

    for (i = 0; i < vectors; i++) {
        int fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        if (fd < 0) {
            set_error(dev, "Eventfd creation failed: %s", strerror(errno));
            rc = NVME_DMA_ERR_BACKEND;
            break;
        }
        dev->irq_fds[i] = fd;
        ((int32_t *)set->data)[i] = fd;
    }

    if (rc == NVME_DMA_OK) {
        set->argsz = (uint32_t)argsz;
        set->flags = VFIO_IRQ_SET_DATA_EVENTFD | VFIO_IRQ_SET_ACTION_TRIGGER;
        set->index = VFIO_PCI_MSIX_IRQ_INDEX;
        set->start = 0;
        set->count = vectors;
        if (dev->ops->ioctl(dev->device_fd, VFIO_DEVICE_SET_IRQS, set) != 0) {
            set_error(dev, "VFIO_DEVICE_SET_IRQS failed: %s", strerror(errno));
            rc = NVME_DMA_ERR_BACKEND;
        }
    }

    if (rc != NVME_DMA_OK) {
        for (i = 0; i < LANTERN_MAX_VECTORS; i++) {
            if (dev->irq_fds[i] >= 0) {
                close(dev->irq_fds[i]);
                dev->irq_fds[i] = -1;
            }
        }
    } else {
        dev->irq_count = vectors;
    }
    free(set);
    return rc;
}

int nvme_irq_disable(nvme_dev_t *dev)
{
    struct vfio_irq_set set;
    unsigned i;

    if (!dev)
        return NVME_DMA_ERR_INVALID;
    if (dev->irq_count == 0)
        return NVME_DMA_OK;

    memset(&set, 0, sizeof(set));
    set.argsz = sizeof(set);
    set.flags = VFIO_IRQ_SET_DATA_NONE | VFIO_IRQ_SET_ACTION_TRIGGER;
    set.index = VFIO_PCI_MSIX_IRQ_INDEX;
    set.start = 0;
    set.count = dev->irq_count;
    dev->ops->ioctl(dev->device_fd, VFIO_DEVICE_SET_IRQS, &set);

    for (i = 0; i < LANTERN_MAX_VECTORS; i++) {
        if (dev->irq_fds[i] >= 0) {
            close(dev->irq_fds[i]);
            dev->irq_fds[i] = -1;
        }
    }
    dev->irq_count = 0;
    return NVME_DMA_OK;
}

int nvme_irq_fd(const nvme_dev_t *dev, unsigned vector)
{
    if (!dev || vector >= LANTERN_MAX_VECTORS)
        return -1;
    return dev->irq_fds[vector];
}

unsigned nvme_irq_count(const nvme_dev_t *dev)
{
    return dev ? dev->irq_count : 0;
}

int nvme_irq_wait(const nvme_dev_t *dev, unsigned vector, int timeout_ms)
{
    struct pollfd pfd;
    uint64_t counter = 0;
    int rc;

    if (!dev || vector >= dev->irq_count)
        return NVME_DMA_ERR_INVALID;

    pfd.fd = dev->irq_fds[vector];
    pfd.events = POLLIN;
    pfd.revents = 0;
    rc = poll(&pfd, 1, timeout_ms);
    if (rc < 0)
        return NVME_DMA_ERR_BACKEND;
    if (rc == 0)
        return 0;
    if (read(pfd.fd, &counter, sizeof(counter)) != (ssize_t)sizeof(counter))
        return NVME_DMA_ERR_BACKEND;
    return (int)(counter > 0x7fffffffull ? 0x7fffffff : counter);
}

static int controller_disable(nvme_dev_t *dev)
{
    uint32_t cc = mmio_read32(dev, NVME_REG_CC);
    uint64_t deadline;
    unsigned spins = 0;

    cc &= ~NVME_CC_EN;
    mmio_write32(dev, NVME_REG_CC, cc);

    deadline = now_ns() + (uint64_t)dev->timeout_ms * 1000000ull;
    while (mmio_read32(dev, NVME_REG_CSTS) & NVME_CSTS_RDY) {
        if (now_ns() > deadline) {
            set_error(dev, "Controller stayed ready %ums after clearing CC.EN", dev->timeout_ms);
            return NVME_DMA_ERR_TIMEOUT;
        }
        relax(spins++);
    }
    return NVME_DMA_OK;
}

static int controller_enable(nvme_dev_t *dev)
{
    uint32_t cc;
    uint64_t deadline;
    unsigned spins = 0;

    cc = NVME_CC_EN;
    cc |= 0u << NVME_CC_CSS_SHIFT;
    cc |= 0u << NVME_CC_MPS_SHIFT;
    cc |= 0u << NVME_CC_AMS_SHIFT;
    cc |= 6u << NVME_CC_IOSQES_SHIFT;
    cc |= 4u << NVME_CC_IOCQES_SHIFT;
    mmio_write32(dev, NVME_REG_CC, cc);

    deadline = now_ns() + (uint64_t)dev->timeout_ms * 1000000ull;
    for (;;) {
        uint32_t csts = mmio_read32(dev, NVME_REG_CSTS);
        if (csts & NVME_CSTS_CFS) {
            set_error(dev, "Controller reported fatal status during enable");
            return NVME_DMA_ERR_CONTROLLER;
        }
        if (csts & NVME_CSTS_RDY)
            break;
        if (now_ns() > deadline) {
            set_error(dev, "Controller did not become ready within %ums", dev->timeout_ms);
            return NVME_DMA_ERR_TIMEOUT;
        }
        relax(spins++);
    }
    return NVME_DMA_OK;
}

static int admin_queue_setup(nvme_dev_t *dev, uint16_t depth)
{
    nvme_queue_t *queue = &dev->queues[0];
    uint32_t aqa;
    int rc;

    rc = queue_alloc_memory(dev, queue, 0, depth);
    if (rc != NVME_DMA_OK)
        return rc;

    aqa = ((uint32_t)(depth - 1u) & 0xfffu) | (((uint32_t)(depth - 1u) & 0xfffu) << 16);
    mmio_write32(dev, NVME_REG_AQA, aqa);
    mmio_write64(dev, NVME_REG_ASQ, queue->sq_mem->iova);
    mmio_write64(dev, NVME_REG_ACQ, queue->cq_mem->iova);
    queue->active = 1;
    return NVME_DMA_OK;
}

static int pci_enable_bus_master(nvme_dev_t *dev)
{
    uint16_t command = 0;

    if (dev->ops->pread(dev->device_fd, &command, sizeof(command),
                        (off_t)(dev->config_region_offset + 0x04)) != (ssize_t)sizeof(command)) {
        set_error(dev, "Reading the PCI command register failed: %s", strerror(errno));
        return NVME_DMA_ERR_BACKEND;
    }
    command |= 0x0002u | 0x0004u;
    if (dev->ops->pwrite(dev->device_fd, &command, sizeof(command),
                         (off_t)(dev->config_region_offset + 0x04)) != (ssize_t)sizeof(command)) {
        set_error(dev, "Writing the PCI command register failed: %s", strerror(errno));
        return NVME_DMA_ERR_BACKEND;
    }
    return NVME_DMA_OK;
}

static int vfio_attach(nvme_dev_t *dev)
{
    struct vfio_group_status group_status;
    struct vfio_device_info device_info;
    struct vfio_region_info region;
    char group_path[64];
    int group_id;
    int api;

    dev->container_fd = dev->ops->open("/dev/vfio/vfio", O_RDWR);
    if (dev->container_fd < 0) {
        set_error(dev, "Opening /dev/vfio/vfio failed: %s", strerror(errno));
        return NVME_DMA_ERR_NO_DEVICE;
    }

    api = dev->ops->ioctl(dev->container_fd, VFIO_GET_API_VERSION, NULL);
    if (api != VFIO_API_VERSION) {
        set_error(dev, "Unexpected VFIO API version %d", api);
        return NVME_DMA_ERR_BACKEND;
    }
    if (dev->ops->ioctl(dev->container_fd, VFIO_CHECK_EXTENSION,
                        (void *)(uintptr_t)VFIO_TYPE1_IOMMU) != 1) {
        set_error(dev, "VFIO type 1 IOMMU unavailable");
        return NVME_DMA_ERR_BACKEND;
    }

    group_id = dev->ops->iommu_group_of(dev->bdf);
    if (group_id < 0) {
        set_error(dev, "No IOMMU group for %s: %s", dev->bdf, strerror(errno));
        return NVME_DMA_ERR_NO_DEVICE;
    }
    snprintf(group_path, sizeof(group_path), "/dev/vfio/%d", group_id);
    dev->group_fd = dev->ops->open(group_path, O_RDWR);
    if (dev->group_fd < 0) {
        set_error(dev, "Opening %s failed: %s", group_path, strerror(errno));
        return NVME_DMA_ERR_NO_DEVICE;
    }

    memset(&group_status, 0, sizeof(group_status));
    group_status.argsz = sizeof(group_status);
    if (dev->ops->ioctl(dev->group_fd, VFIO_GROUP_GET_STATUS, &group_status) != 0) {
        set_error(dev, "VFIO_GROUP_GET_STATUS failed: %s", strerror(errno));
        return NVME_DMA_ERR_BACKEND;
    }
    if (!(group_status.flags & VFIO_GROUP_FLAGS_VIABLE)) {
        set_error(dev, "IOMMU group %d is not viable", group_id);
        return NVME_DMA_ERR_NO_DEVICE;
    }
    if (dev->ops->ioctl(dev->group_fd, VFIO_GROUP_SET_CONTAINER, &dev->container_fd) != 0) {
        set_error(dev, "VFIO_GROUP_SET_CONTAINER failed: %s", strerror(errno));
        return NVME_DMA_ERR_BACKEND;
    }
    if (dev->ops->ioctl(dev->container_fd, VFIO_SET_IOMMU,
                        (void *)(uintptr_t)VFIO_TYPE1_IOMMU) != 0) {
        set_error(dev, "VFIO_SET_IOMMU failed: %s", strerror(errno));
        return NVME_DMA_ERR_BACKEND;
    }

    dev->device_fd = dev->ops->ioctl(dev->group_fd, VFIO_GROUP_GET_DEVICE_FD, (void *)dev->bdf);
    if (dev->device_fd < 0) {
        set_error(dev, "VFIO_GROUP_GET_DEVICE_FD for %s failed: %s", dev->bdf, strerror(errno));
        return NVME_DMA_ERR_NO_DEVICE;
    }

    memset(&device_info, 0, sizeof(device_info));
    device_info.argsz = sizeof(device_info);
    if (dev->ops->ioctl(dev->device_fd, VFIO_DEVICE_GET_INFO, &device_info) != 0) {
        set_error(dev, "VFIO_DEVICE_GET_INFO failed: %s", strerror(errno));
        return NVME_DMA_ERR_BACKEND;
    }
    if (device_info.num_regions <= VFIO_PCI_BAR0_REGION_INDEX) {
        set_error(dev, "Device exposes %u regions, BAR0 missing", device_info.num_regions);
        return NVME_DMA_ERR_BACKEND;
    }

    memset(&region, 0, sizeof(region));
    region.argsz = sizeof(region);
    region.index = VFIO_PCI_CONFIG_REGION_INDEX;
    if (dev->ops->ioctl(dev->device_fd, VFIO_DEVICE_GET_REGION_INFO, &region) != 0) {
        set_error(dev, "VFIO_DEVICE_GET_REGION_INFO(config) failed: %s", strerror(errno));
        return NVME_DMA_ERR_BACKEND;
    }
    dev->config_region_offset = region.offset;

    memset(&region, 0, sizeof(region));
    region.argsz = sizeof(region);
    region.index = VFIO_PCI_BAR0_REGION_INDEX;
    if (dev->ops->ioctl(dev->device_fd, VFIO_DEVICE_GET_REGION_INFO, &region) != 0) {
        set_error(dev, "VFIO_DEVICE_GET_REGION_INFO(bar0) failed: %s", strerror(errno));
        return NVME_DMA_ERR_BACKEND;
    }
    if (!(region.flags & VFIO_REGION_INFO_FLAG_MMAP)) {
        set_error(dev, "BAR0 is not mappable");
        return NVME_DMA_ERR_BACKEND;
    }
    dev->bar_size = region.size;
    dev->bar_region_offset = region.offset;
    dev->bar = dev->ops->mmap(NULL, dev->bar_size, PROT_READ | PROT_WRITE, MAP_SHARED,
                              dev->device_fd, (off_t)region.offset);
    if (dev->bar == MAP_FAILED) {
        dev->bar = NULL;
        set_error(dev, "Mapping BAR0 failed: %s", strerror(errno));
        return NVME_DMA_ERR_BACKEND;
    }

    if (dev->ops->ioctl(dev->device_fd, VFIO_DEVICE_RESET, NULL) != 0) {
        set_error(dev, "VFIO_DEVICE_RESET failed: %s", strerror(errno));
        return NVME_DMA_ERR_BACKEND;
    }

    return pci_enable_bus_master(dev);
}

void nvme_dev_config_defaults(nvme_dev_config_t *config)
{
    memset(config, 0, sizeof(*config));
    config->backend = NVME_DMA_BACKEND_MOCK;
    config->admin_queue_depth = 64;
    config->enable_msix = 0;
    config->mock_image_path = "mock.img";
    config->mock_capacity_bytes = 64ull * 1024ull * 1024ull;
    config->mock_lba_bytes = 512;
    config->mock_namespaces = 4;
    config->mock_latency_us = 0;
    config->mock_serial = "DEADBEEF";
    config->mock_model = "lantern mock nvme";
}

int nvme_dev_open(const char *bdf, const nvme_dev_config_t *config, nvme_dev_t **out)
{
    nvme_dev_config_t defaults;
    nvme_dev_t *dev;
    int rc;
    int i;

    if (!bdf || !out)
        return NVME_DMA_ERR_INVALID;
    if (strlen(bdf) != 12)
        return NVME_DMA_ERR_INVALID;

    if (!config) {
        nvme_dev_config_defaults(&defaults);
        config = &defaults;
    }

    dev = calloc(1, sizeof(*dev));
    if (!dev)
        return NVME_DMA_ERR_NOMEM;

    snprintf(dev->bdf, sizeof(dev->bdf), "%s", bdf);
    dev->backend = config->backend;
    dev->container_fd = -1;
    dev->group_fd = -1;
    dev->device_fd = -1;
    for (i = 0; i < LANTERN_MAX_VECTORS; i++)
        dev->irq_fds[i] = -1;

    if (config->backend == NVME_DMA_BACKEND_MOCK) {
        lt_mock_config_t mock;
        lt_mock_config_defaults(&mock);
        if (config->mock_image_path)
            mock.image_path = config->mock_image_path;
        if (config->mock_capacity_bytes)
            mock.capacity_bytes = config->mock_capacity_bytes;
        if (config->mock_lba_bytes)
            mock.lba_bytes = config->mock_lba_bytes;
        if (config->mock_namespaces)
            mock.max_namespaces = config->mock_namespaces;
        if (config->mock_serial)
            mock.serial = config->mock_serial;
        if (config->mock_model)
            mock.model = config->mock_model;
        mock.queue_latency_us = config->mock_latency_us;
        if (lt_mock_arm(&mock) != 0) {
            set_error(dev, "Mock backend refused to start: %s", lt_mock_last_error());
            free(dev);
            return NVME_DMA_ERR_BACKEND;
        }
        dev->ops = lt_syscalls_mock();
    } else {
        dev->ops = lt_syscalls_real();
    }

    rc = vfio_attach(dev);
    if (rc != NVME_DMA_OK) {
        char saved[LANTERN_ERROR_LEN];
        snprintf(saved, sizeof(saved), "%s", dev->error);
        nvme_dev_close(dev);
        *out = NULL;
        fprintf(stderr, "lantern: %s\n", saved);
        return rc;
    }

    dev->cap = mmio_read64(dev, NVME_REG_CAP);
    dev->doorbell_stride = NVME_CAP_DSTRD(dev->cap);
    dev->max_queue_entries = NVME_CAP_MQES(dev->cap);
    dev->timeout_ms = NVME_CAP_TO(dev->cap) * 500u;
    if (dev->timeout_ms == 0)
        dev->timeout_ms = 500u;
    dev->page_size = NVME_PAGE_SIZE;
    dev->max_transfer_bytes = NVME_PAGE_SIZE * LANTERN_PRP_PER_PAGE;

    if (NVME_CAP_MPSMIN(dev->cap) != 0) {
        set_error(dev, "Controller minimum page size exceeds the host page size");
        nvme_dev_close(dev);
        *out = NULL;
        return NVME_DMA_ERR_CONTROLLER;
    }

    rc = controller_disable(dev);
    if (rc != NVME_DMA_OK) {
        nvme_dev_close(dev);
        *out = NULL;
        return rc;
    }

    {
        uint16_t depth = config->admin_queue_depth ? config->admin_queue_depth : 64;
        if (depth > dev->max_queue_entries)
            depth = (uint16_t)dev->max_queue_entries;
        if (depth > 4096)
            depth = 4096;
        rc = admin_queue_setup(dev, depth);
    }
    if (rc != NVME_DMA_OK) {
        nvme_dev_close(dev);
        *out = NULL;
        return rc;
    }

    if (config->enable_msix) {
        rc = nvme_irq_enable(dev, NVME_DMA_MAX_QUEUES);
        if (rc != NVME_DMA_OK) {
            nvme_dev_close(dev);
            *out = NULL;
            return rc;
        }
    }

    rc = controller_enable(dev);
    if (rc != NVME_DMA_OK) {
        nvme_dev_close(dev);
        *out = NULL;
        return rc;
    }

    *out = dev;
    return NVME_DMA_OK;
}

int nvme_dev_reset(nvme_dev_t *dev)
{
    uint16_t qid;
    uint16_t admin_depth;
    int rc;

    if (!dev)
        return NVME_DMA_ERR_INVALID;

    admin_depth = dev->queues[0].depth;
    rc = controller_disable(dev);
    if (rc != NVME_DMA_OK)
        return rc;

    for (qid = 1; qid < NVME_DMA_MAX_QUEUES; qid++)
        queue_release(dev, &dev->queues[qid]);
    queue_release(dev, &dev->queues[0]);

    rc = admin_queue_setup(dev, admin_depth);
    if (rc != NVME_DMA_OK)
        return rc;

    return controller_enable(dev);
}

int nvme_dev_close(nvme_dev_t *dev)
{
    uint16_t qid;
    int i;

    if (!dev)
        return NVME_DMA_ERR_INVALID;

    if (dev->bar)
        controller_disable(dev);

    nvme_irq_disable(dev);

    for (qid = 0; qid < NVME_DMA_MAX_QUEUES; qid++)
        queue_release(dev, &dev->queues[qid]);

    for (i = 0; i < LANTERN_MAX_DMA; i++)
        if (dev->dma[i].in_use)
            nvme_dma_free(dev, &dev->dma[i]);

    if (dev->bar)
        dev->ops->munmap(dev->bar, dev->bar_size);
    if (dev->device_fd >= 0)
        dev->ops->close(dev->device_fd);
    if (dev->group_fd >= 0)
        dev->ops->close(dev->group_fd);
    if (dev->container_fd >= 0)
        dev->ops->close(dev->container_fd);

    if (dev->backend == NVME_DMA_BACKEND_MOCK)
        lt_mock_disarm();

    free(dev);
    return NVME_DMA_OK;
}

const char *nvme_dev_backend_name(const nvme_dev_t *dev)
{
    return dev && dev->ops ? dev->ops->name : "none";
}

const char *nvme_dev_bdf(const nvme_dev_t *dev)
{
    return dev ? dev->bdf : "";
}

const char *nvme_dev_error(const nvme_dev_t *dev)
{
    return dev ? dev->error : "";
}

const char *nvme_strerror(int code)
{
    switch (code) {
    case NVME_DMA_OK:              return "OK";
    case NVME_DMA_ERR_INVALID:     return "Invalid argument";
    case NVME_DMA_ERR_BACKEND:     return "VFIO backend failure";
    case NVME_DMA_ERR_NO_DEVICE:   return "Device not available";
    case NVME_DMA_ERR_TIMEOUT:     return "Timed out";
    case NVME_DMA_ERR_NOMEM:       return "Out of memory";
    case NVME_DMA_ERR_CONTROLLER:  return "Controller fatal status";
    case NVME_DMA_ERR_QUEUE_FULL:  return "Submission queue full";
    case NVME_DMA_ERR_STATE:       return "Invalid state";
    case NVME_DMA_ERR_RANGE:       return "Range outside buffer";
    default:                       return "Unknown error";
    }
}

int nvme_dev_state(const nvme_dev_t *dev, nvme_dev_state_t *state)
{
    if (!dev || !state)
        return NVME_DMA_ERR_INVALID;

    state->cap = mmio_read64(dev, NVME_REG_CAP);
    state->vs = mmio_read32(dev, NVME_REG_VS);
    state->cc = mmio_read32(dev, NVME_REG_CC);
    state->csts = mmio_read32(dev, NVME_REG_CSTS);
    state->aqa = mmio_read32(dev, NVME_REG_AQA);
    state->asq = mmio_read64(dev, NVME_REG_ASQ);
    state->acq = mmio_read64(dev, NVME_REG_ACQ);
    state->max_queue_entries = dev->max_queue_entries;
    state->doorbell_stride = dev->doorbell_stride;
    state->timeout_ms = dev->timeout_ms;
    state->page_size = dev->page_size;
    state->max_transfer_bytes = dev->max_transfer_bytes;
    return NVME_DMA_OK;
}

int nvme_queue_state(const nvme_dev_t *dev, uint16_t qid, nvme_queue_state_t *state)
{
    const nvme_queue_t *queue;

    if (!dev || !state || qid >= NVME_DMA_MAX_QUEUES)
        return NVME_DMA_ERR_INVALID;

    queue = &dev->queues[qid];
    state->active = queue->active;
    state->qid = qid;
    state->depth = queue->depth;
    state->sq_tail = queue->sq_tail;
    state->cq_head = queue->cq_head;
    state->cq_phase = queue->cq_phase;
    state->sq_iova = queue->sq_mem ? queue->sq_mem->iova : 0;
    state->cq_iova = queue->cq_mem ? queue->cq_mem->iova : 0;
    state->submitted = queue->submitted;
    state->completed = queue->completed;
    return NVME_DMA_OK;
}
