#define _GNU_SOURCE
#include "model.h"
#include "spec.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define MODEL_MAX_QUEUES     17
#define MODEL_MAX_NAMESPACES 16
#define MODEL_MAX_DMA_MAPS   256
#define MODEL_MAX_MSIX       32
#define MODEL_BAR_SIZE       8192
#define MODEL_CONFIG_SIZE    256
#define MODEL_MDTS_SHIFT     9
#define MODEL_CQ_STALL_LIMIT 2000000

typedef struct {
    int      valid;
    uint64_t iova;
    uint64_t vaddr;
    uint64_t size;
} model_dma_map_t;

typedef struct {
    int      active;
    uint16_t depth;
    uint16_t cqid;
    uint16_t head;
    uint64_t base_iova;
} model_sq_t;

typedef struct {
    int      active;
    uint16_t depth;
    uint16_t tail;
    uint8_t  phase;
    int      irq_enabled;
    uint16_t irq_vector;
    uint64_t base_iova;
} model_cq_t;

typedef struct {
    int      allocated;
    int      attached;
    uint64_t offset_bytes;
    uint64_t size_bytes;
    uint32_t lba_bytes;
} model_ns_t;

struct nvme_model {
    uint8_t  *bar;
    uint8_t   config[MODEL_CONFIG_SIZE];
    int       image_fd;
    uint64_t  capacity_bytes;
    uint32_t  lba_bytes;
    uint32_t  max_namespaces;
    uint32_t  latency_us;
    char      serial[21];
    char      model_name[41];

    model_dma_map_t maps[MODEL_MAX_DMA_MAPS];
    pthread_mutex_t map_lock;

    model_sq_t sq[MODEL_MAX_QUEUES];
    model_cq_t cq[MODEL_MAX_QUEUES];
    model_ns_t ns[MODEL_MAX_NAMESPACES + 1];

    int32_t  msix_fds[MODEL_MAX_MSIX];
    uint32_t msix_count;

    uint64_t data_units_read;
    uint64_t data_units_written;
    uint64_t commands_executed;

    int       enabled;
    pthread_t thread;
    volatile int stop;
};

static uint32_t reg_load(nvme_model_t *m, uint32_t off)
{
    return __atomic_load_n((uint32_t *)(m->bar + off), __ATOMIC_ACQUIRE);
}

static void reg_store(nvme_model_t *m, uint32_t off, uint32_t value)
{
    __atomic_store_n((uint32_t *)(m->bar + off), value, __ATOMIC_RELEASE);
}

static uint64_t reg_load64(nvme_model_t *m, uint32_t off)
{
    uint64_t lo = reg_load(m, off);
    uint64_t hi = reg_load(m, off + 4);
    return lo | (hi << 32);
}

static void reg_store64(nvme_model_t *m, uint32_t off, uint64_t value)
{
    reg_store(m, off, (uint32_t)value);
    reg_store(m, off + 4, (uint32_t)(value >> 32));
}

static uint32_t sq_doorbell(nvme_model_t *m, uint16_t qid)
{
    return reg_load(m, NVME_REG_DOORBELL_BASE + (uint32_t)qid * 8u);
}

static uint32_t cq_doorbell(nvme_model_t *m, uint16_t qid)
{
    return reg_load(m, NVME_REG_DOORBELL_BASE + (uint32_t)qid * 8u + 4u);
}

static uint16_t make_status(uint8_t sct, uint8_t sc, int dnr)
{
    return (uint16_t)(((dnr ? 1u : 0u) << 15) | ((uint32_t)(sct & 0x7u) << 9) | ((uint32_t)sc << 1));
}

static void *model_xlate(nvme_model_t *m, uint64_t iova, uint64_t len)
{
    void *result = NULL;
    int i;

    pthread_mutex_lock(&m->map_lock);
    for (i = 0; i < MODEL_MAX_DMA_MAPS; i++) {
        model_dma_map_t *map = &m->maps[i];
        if (!map->valid)
            continue;
        if (iova >= map->iova && iova + len <= map->iova + map->size) {
            result = (void *)(uintptr_t)(map->vaddr + (iova - map->iova));
            break;
        }
    }
    pthread_mutex_unlock(&m->map_lock);
    return result;
}

int nvme_model_dma_map(nvme_model_t *m, uint64_t iova, uint64_t vaddr, uint64_t size)
{
    int i;
    int slot = -1;

    pthread_mutex_lock(&m->map_lock);
    for (i = 0; i < MODEL_MAX_DMA_MAPS; i++) {
        model_dma_map_t *map = &m->maps[i];
        if (map->valid && iova < map->iova + map->size && map->iova < iova + size) {
            pthread_mutex_unlock(&m->map_lock);
            errno = EEXIST;
            return -1;
        }
        if (!map->valid && slot < 0)
            slot = i;
    }
    if (slot < 0) {
        pthread_mutex_unlock(&m->map_lock);
        errno = ENOSPC;
        return -1;
    }
    m->maps[slot].valid = 1;
    m->maps[slot].iova = iova;
    m->maps[slot].vaddr = vaddr;
    m->maps[slot].size = size;
    pthread_mutex_unlock(&m->map_lock);
    return 0;
}

int nvme_model_dma_unmap(nvme_model_t *m, uint64_t iova, uint64_t size)
{
    int i;
    int found = 0;

    pthread_mutex_lock(&m->map_lock);
    for (i = 0; i < MODEL_MAX_DMA_MAPS; i++) {
        model_dma_map_t *map = &m->maps[i];
        if (map->valid && map->iova >= iova && map->iova + map->size <= iova + size) {
            map->valid = 0;
            found = 1;
        }
    }
    pthread_mutex_unlock(&m->map_lock);
    if (!found) {
        errno = ENOENT;
        return -1;
    }
    return 0;
}

int nvme_model_set_msix(nvme_model_t *m, uint32_t start, uint32_t count, const int32_t *fds)
{
    uint32_t i;

    if (start + count > MODEL_MAX_MSIX) {
        errno = EINVAL;
        return -1;
    }
    for (i = 0; i < count; i++)
        m->msix_fds[start + i] = fds ? fds[i] : -1;
    if (fds && start + count > m->msix_count)
        m->msix_count = start + count;
    if (!fds && start == 0 && count >= m->msix_count)
        m->msix_count = 0;
    return 0;
}

uint32_t nvme_model_msix_count(nvme_model_t *m)
{
    return m->msix_count;
}

uint64_t nvme_model_commands_executed(nvme_model_t *m)
{
    return __atomic_load_n(&m->commands_executed, __ATOMIC_RELAXED);
}

static void model_signal_irq(nvme_model_t *m, uint16_t vector)
{
    uint64_t one = 1;
    int32_t fd;

    if (vector >= MODEL_MAX_MSIX)
        return;
    fd = m->msix_fds[vector];
    if (fd < 0)
        return;
    if (write(fd, &one, sizeof(one)) != (ssize_t)sizeof(one))
        return;
}

static int model_post_completion(nvme_model_t *m, uint16_t cqid, uint16_t sqid,
                                 uint16_t sq_head, uint16_t cid, uint32_t dw0, uint16_t status)
{
    model_cq_t *cq = &m->cq[cqid];
    struct nvme_wire_cqe *slot;
    uint16_t next;
    uint64_t spins = 0;

    if (!cq->active)
        return -1;

    next = (uint16_t)((cq->tail + 1u) % cq->depth);
    while (next == (uint16_t)cq_doorbell(m, cqid)) {
        if (++spins > MODEL_CQ_STALL_LIMIT) {
            reg_store(m, NVME_REG_CSTS, reg_load(m, NVME_REG_CSTS) | NVME_CSTS_CFS);
            return -1;
        }
        sched_yield();
    }

    slot = (struct nvme_wire_cqe *)model_xlate(m, cq->base_iova + (uint64_t)cq->tail * NVME_CQE_BYTES,
                                               NVME_CQE_BYTES);
    if (!slot)
        return -1;

    slot->dw0 = dw0;
    slot->dw1 = 0;
    slot->sq_head = sq_head;
    slot->sq_id = sqid;
    slot->cid = cid;
    __atomic_store_n(&slot->status, (uint16_t)(status | cq->phase), __ATOMIC_RELEASE);

    cq->tail = next;
    if (cq->tail == 0)
        cq->phase ^= 1u;

    if (cq->irq_enabled)
        model_signal_irq(m, cq->irq_vector);
    return 0;
}

static int model_prp_transfer(nvme_model_t *m, uint64_t prp1, uint64_t prp2,
                              uint64_t total, int to_device, uint64_t media_offset,
                              uint16_t *status)
{
    uint64_t remaining = total;
    uint64_t offset_in_page = prp1 & NVME_PAGE_MASK;
    uint64_t cursor_iova = prp1;
    uint64_t chunk = NVME_PAGE_SIZE - offset_in_page;
    uint64_t *list = NULL;
    uint32_t list_index = 0;
    uint64_t media = media_offset;

    if (remaining == 0)
        return 0;
    if (chunk > remaining)
        chunk = remaining;

    for (;;) {
        void *host = model_xlate(m, cursor_iova, chunk);
        ssize_t done;

        if (!host) {
            *status = make_status(NVME_SCT_GENERIC, NVME_SC_DATA_TRANSFER_ERROR, 1);
            return -1;
        }
        if (to_device)
            done = pwrite(m->image_fd, host, chunk, (off_t)media);
        else
            done = pread(m->image_fd, host, chunk, (off_t)media);
        if (done != (ssize_t)chunk) {
            *status = make_status(NVME_SCT_GENERIC, NVME_SC_INTERNAL_ERROR, 1);
            return -1;
        }
        media += chunk;
        remaining -= chunk;
        if (remaining == 0)
            break;

        if (list == NULL) {
            if (remaining <= NVME_PAGE_SIZE) {
                if (prp2 & NVME_PAGE_MASK) {
                    *status = make_status(NVME_SCT_GENERIC, NVME_SC_INVALID_FIELD, 1);
                    return -1;
                }
                cursor_iova = prp2;
                chunk = remaining;
                continue;
            }
            if (prp2 & NVME_PAGE_MASK) {
                *status = make_status(NVME_SCT_GENERIC, NVME_SC_INVALID_FIELD, 1);
                return -1;
            }
            list = (uint64_t *)model_xlate(m, prp2, NVME_PAGE_SIZE);
            if (!list) {
                *status = make_status(NVME_SCT_GENERIC, NVME_SC_DATA_TRANSFER_ERROR, 1);
                return -1;
            }
            list_index = 0;
        } else if (list_index == (NVME_PAGE_SIZE / sizeof(uint64_t)) - 1) {
            uint64_t next_list = list[list_index];
            if (next_list & NVME_PAGE_MASK) {
                *status = make_status(NVME_SCT_GENERIC, NVME_SC_INVALID_FIELD, 1);
                return -1;
            }
            list = (uint64_t *)model_xlate(m, next_list, NVME_PAGE_SIZE);
            if (!list) {
                *status = make_status(NVME_SCT_GENERIC, NVME_SC_DATA_TRANSFER_ERROR, 1);
                return -1;
            }
            list_index = 0;
        }

        cursor_iova = list[list_index++];
        if (cursor_iova & NVME_PAGE_MASK) {
            *status = make_status(NVME_SCT_GENERIC, NVME_SC_INVALID_FIELD, 1);
            return -1;
        }
        chunk = remaining < NVME_PAGE_SIZE ? remaining : NVME_PAGE_SIZE;
    }
    return 0;
}

static void model_fill_id_ctrl(nvme_model_t *m, struct nvme_id_ctrl *id)
{
    uint64_t total = m->capacity_bytes;

    memset(id, 0, sizeof(*id));
    id->vid = 0x1b36;
    id->ssvid = 0x1af4;
    memset(id->sn, ' ', sizeof(id->sn));
    memcpy(id->sn, m->serial, strnlen(m->serial, sizeof(id->sn)));
    memset(id->mn, ' ', sizeof(id->mn));
    memcpy(id->mn, m->model_name, strnlen(m->model_name, sizeof(id->mn)));
    memcpy(id->fr, "lntrn1.0", 8);
    id->rab = 6;
    id->ieee[0] = 0x52;
    id->ieee[1] = 0x54;
    id->ieee[2] = 0x00;
    id->cmic = 0;
    id->mdts = MODEL_MDTS_SHIFT;
    id->cntlid = 1;
    id->ver = 0x00010400;
    id->oacs = 0x0008;
    id->acl = 3;
    id->aerl = 3;
    id->frmw = 0x02;
    id->lpa = 0x02;
    id->elpe = 3;
    id->wctemp = 343;
    id->cctemp = 353;
    id->sqes = 0x66;
    id->cqes = 0x44;
    id->maxcmd = 1024;
    id->nn = m->max_namespaces;
    id->oncs = 0x0008;
    id->fna = 0x00;
    id->vwc = 0x01;
    id->awun = 0;
    id->awupf = 0;
    memcpy(id->vs, &total, sizeof(total));
}

static void model_fill_id_ns(nvme_model_t *m, uint32_t nsid, struct nvme_id_ns *id)
{
    model_ns_t *ns = &m->ns[nsid];
    uint64_t lbas = ns->size_bytes / ns->lba_bytes;

    memset(id, 0, sizeof(*id));
    id->nsze = lbas;
    id->ncap = lbas;
    id->nuse = lbas;
    id->nsfeat = 0;
    id->nlbaf = 1;
    id->flbas = ns->lba_bytes == 4096 ? 1 : 0;
    id->mc = 0;
    id->dpc = 0;
    id->dps = 0;
    id->nmic = 0;
    id->dlfeat = 0x09;
    id->nvmcap[0] = ns->size_bytes;
    id->lbaf[0].lbads = 9;
    id->lbaf[0].rp = 0;
    id->lbaf[1].lbads = 12;
    id->lbaf[1].rp = 0;
    id->nguid[0] = 0x52;
    id->nguid[1] = 0x54;
    id->nguid[15] = (uint8_t)nsid;
    id->eui64[7] = (uint8_t)nsid;
}

static int model_ns_valid(nvme_model_t *m, uint32_t nsid)
{
    if (nsid == 0 || nsid > m->max_namespaces || nsid > MODEL_MAX_NAMESPACES)
        return 0;
    return m->ns[nsid].allocated && m->ns[nsid].attached;
}

static uint64_t model_allocated_bytes(nvme_model_t *m)
{
    uint64_t used = 0;
    uint32_t i;

    for (i = 1; i <= m->max_namespaces && i <= MODEL_MAX_NAMESPACES; i++)
        if (m->ns[i].allocated)
            used += m->ns[i].size_bytes;
    return used;
}

static int model_ns_place(nvme_model_t *m, uint64_t size_bytes, uint64_t *offset_out)
{
    uint64_t cursor = 0;
    int progress = 1;

    if (size_bytes > m->capacity_bytes)
        return -1;
    while (progress) {
        uint32_t i;
        progress = 0;
        for (i = 1; i <= m->max_namespaces && i <= MODEL_MAX_NAMESPACES; i++) {
            model_ns_t *ns = &m->ns[i];
            if (!ns->allocated)
                continue;
            if (cursor < ns->offset_bytes + ns->size_bytes && ns->offset_bytes < cursor + size_bytes) {
                cursor = ns->offset_bytes + ns->size_bytes;
                progress = 1;
            }
        }
    }
    if (cursor + size_bytes > m->capacity_bytes)
        return -1;
    *offset_out = cursor;
    return 0;
}

static void model_fill_smart(nvme_model_t *m, uint8_t *page)
{
    memset(page, 0, 512);
    page[0] = 0;
    page[1] = 0x39;
    page[2] = 0x01;
    page[3] = 100;
    page[4] = 10;
    page[5] = 1;
    memcpy(page + 32, &m->data_units_read, sizeof(uint64_t));
    memcpy(page + 48, &m->data_units_written, sizeof(uint64_t));
    memcpy(page + 64, &m->commands_executed, sizeof(uint64_t));
    memcpy(page + 80, &m->commands_executed, sizeof(uint64_t));
}

static uint16_t model_exec_admin(nvme_model_t *m, const struct nvme_wire_sqe *cmd, uint32_t *dw0)
{
    *dw0 = 0;

    switch (cmd->opcode) {
    case NVME_ADMIN_IDENTIFY: {
        uint32_t cns = cmd->cdw10 & 0xffu;
        void *host = model_xlate(m, cmd->prp1, NVME_PAGE_SIZE);
        if (!host)
            return make_status(NVME_SCT_GENERIC, NVME_SC_DATA_TRANSFER_ERROR, 1);
        if (cns == NVME_IDENTIFY_CNS_CONTROLLER) {
            struct nvme_id_ctrl id;
            model_fill_id_ctrl(m, &id);
            memcpy(host, &id, sizeof(id));
            return make_status(NVME_SCT_GENERIC, NVME_SC_SUCCESS, 0);
        }
        if (cns == NVME_IDENTIFY_CNS_NAMESPACE) {
            struct nvme_id_ns id;
            if (!model_ns_valid(m, cmd->nsid))
                return make_status(NVME_SCT_GENERIC, NVME_SC_INVALID_NAMESPACE, 1);
            model_fill_id_ns(m, cmd->nsid, &id);
            memcpy(host, &id, sizeof(id));
            return make_status(NVME_SCT_GENERIC, NVME_SC_SUCCESS, 0);
        }
        if (cns == NVME_IDENTIFY_CNS_NS_LIST) {
            uint32_t *list = (uint32_t *)host;
            uint32_t count = 0;
            uint32_t i;
            memset(host, 0, NVME_PAGE_SIZE);
            for (i = cmd->nsid + 1; i <= m->max_namespaces && i <= MODEL_MAX_NAMESPACES; i++)
                if (model_ns_valid(m, i) && count < 1024)
                    list[count++] = i;
            return make_status(NVME_SCT_GENERIC, NVME_SC_SUCCESS, 0);
        }
        return make_status(NVME_SCT_GENERIC, NVME_SC_INVALID_FIELD, 1);
    }

    case NVME_ADMIN_CREATE_CQ: {
        uint16_t qid = (uint16_t)(cmd->cdw10 & 0xffffu);
        uint16_t qsize = (uint16_t)(((cmd->cdw10 >> 16) & 0xffffu) + 1u);
        uint16_t vector = (uint16_t)((cmd->cdw11 >> 16) & 0xffffu);
        int ien = (int)((cmd->cdw11 >> 1) & 0x1u);
        int pc = (int)(cmd->cdw11 & 0x1u);

        if (qid == 0 || qid >= MODEL_MAX_QUEUES)
            return make_status(NVME_SCT_COMMAND_SPECIFIC, NVME_SC_CS_INVALID_QUEUE_IDENTIFIER, 1);
        if (m->cq[qid].active)
            return make_status(NVME_SCT_COMMAND_SPECIFIC, NVME_SC_CS_INVALID_QUEUE_IDENTIFIER, 1);
        if (qsize < 2 || qsize > NVME_CAP_MQES(reg_load64(m, NVME_REG_CAP)))
            return make_status(NVME_SCT_COMMAND_SPECIFIC, NVME_SC_CS_INVALID_QUEUE_SIZE, 1);
        if (!pc)
            return make_status(NVME_SCT_GENERIC, NVME_SC_INVALID_FIELD, 1);
        if (!model_xlate(m, cmd->prp1, (uint64_t)qsize * NVME_CQE_BYTES))
            return make_status(NVME_SCT_GENERIC, NVME_SC_DATA_TRANSFER_ERROR, 1);
        m->cq[qid].depth = qsize;
        m->cq[qid].base_iova = cmd->prp1;
        m->cq[qid].tail = 0;
        m->cq[qid].phase = 1;
        m->cq[qid].irq_enabled = ien;
        m->cq[qid].irq_vector = vector;
        m->cq[qid].active = 1;
        return make_status(NVME_SCT_GENERIC, NVME_SC_SUCCESS, 0);
    }

    case NVME_ADMIN_CREATE_SQ: {
        uint16_t qid = (uint16_t)(cmd->cdw10 & 0xffffu);
        uint16_t qsize = (uint16_t)(((cmd->cdw10 >> 16) & 0xffffu) + 1u);
        uint16_t cqid = (uint16_t)((cmd->cdw11 >> 16) & 0xffffu);
        int pc = (int)(cmd->cdw11 & 0x1u);

        if (qid == 0 || qid >= MODEL_MAX_QUEUES)
            return make_status(NVME_SCT_COMMAND_SPECIFIC, NVME_SC_CS_INVALID_QUEUE_IDENTIFIER, 1);
        if (m->sq[qid].active)
            return make_status(NVME_SCT_COMMAND_SPECIFIC, NVME_SC_CS_INVALID_QUEUE_IDENTIFIER, 1);
        if (cqid == 0 || cqid >= MODEL_MAX_QUEUES || !m->cq[cqid].active)
            return make_status(NVME_SCT_COMMAND_SPECIFIC, NVME_SC_CS_COMPLETION_QUEUE_INVALID, 1);
        if (qsize < 2 || qsize > NVME_CAP_MQES(reg_load64(m, NVME_REG_CAP)))
            return make_status(NVME_SCT_COMMAND_SPECIFIC, NVME_SC_CS_INVALID_QUEUE_SIZE, 1);
        if (!pc)
            return make_status(NVME_SCT_GENERIC, NVME_SC_INVALID_FIELD, 1);
        if (!model_xlate(m, cmd->prp1, (uint64_t)qsize * NVME_SQE_BYTES))
            return make_status(NVME_SCT_GENERIC, NVME_SC_DATA_TRANSFER_ERROR, 1);
        m->sq[qid].depth = qsize;
        m->sq[qid].base_iova = cmd->prp1;
        m->sq[qid].head = 0;
        m->sq[qid].cqid = cqid;
        m->sq[qid].active = 1;
        return make_status(NVME_SCT_GENERIC, NVME_SC_SUCCESS, 0);
    }

    case NVME_ADMIN_DELETE_SQ: {
        uint16_t qid = (uint16_t)(cmd->cdw10 & 0xffffu);
        if (qid == 0 || qid >= MODEL_MAX_QUEUES || !m->sq[qid].active)
            return make_status(NVME_SCT_COMMAND_SPECIFIC, NVME_SC_CS_INVALID_QUEUE_IDENTIFIER, 1);
        m->sq[qid].active = 0;
        return make_status(NVME_SCT_GENERIC, NVME_SC_SUCCESS, 0);
    }

    case NVME_ADMIN_DELETE_CQ: {
        uint16_t qid = (uint16_t)(cmd->cdw10 & 0xffffu);
        uint16_t i;
        if (qid == 0 || qid >= MODEL_MAX_QUEUES || !m->cq[qid].active)
            return make_status(NVME_SCT_COMMAND_SPECIFIC, NVME_SC_CS_INVALID_QUEUE_IDENTIFIER, 1);
        for (i = 1; i < MODEL_MAX_QUEUES; i++)
            if (m->sq[i].active && m->sq[i].cqid == qid)
                return make_status(NVME_SCT_COMMAND_SPECIFIC, NVME_SC_CS_INVALID_QUEUE_DELETION, 1);
        m->cq[qid].active = 0;
        return make_status(NVME_SCT_GENERIC, NVME_SC_SUCCESS, 0);
    }

    case NVME_ADMIN_SET_FEATURES: {
        uint32_t fid = cmd->cdw10 & 0xffu;
        if (fid == NVME_FEATURE_NUM_QUEUES) {
            uint32_t requested_sq = (cmd->cdw11 & 0xffffu) + 1u;
            uint32_t requested_cq = ((cmd->cdw11 >> 16) & 0xffffu) + 1u;
            uint32_t granted_sq = requested_sq > MODEL_MAX_QUEUES - 1 ? MODEL_MAX_QUEUES - 1 : requested_sq;
            uint32_t granted_cq = requested_cq > MODEL_MAX_QUEUES - 1 ? MODEL_MAX_QUEUES - 1 : requested_cq;
            *dw0 = ((granted_cq - 1u) << 16) | (granted_sq - 1u);
            return make_status(NVME_SCT_GENERIC, NVME_SC_SUCCESS, 0);
        }
        return make_status(NVME_SCT_GENERIC, NVME_SC_INVALID_FIELD, 1);
    }

    case NVME_ADMIN_GET_FEATURES: {
        uint32_t fid = cmd->cdw10 & 0xffu;
        if (fid == NVME_FEATURE_NUM_QUEUES) {
            *dw0 = ((MODEL_MAX_QUEUES - 2u) << 16) | (MODEL_MAX_QUEUES - 2u);
            return make_status(NVME_SCT_GENERIC, NVME_SC_SUCCESS, 0);
        }
        return make_status(NVME_SCT_GENERIC, NVME_SC_INVALID_FIELD, 1);
    }

    case NVME_ADMIN_GET_LOG: {
        uint32_t lid = cmd->cdw10 & 0xffu;
        uint32_t numd = ((cmd->cdw10 >> 16) & 0xffffu) + 1u;
        uint64_t bytes = (uint64_t)numd * 4u;
        uint8_t page[512];
        void *host;

        if (lid != 0x02)
            return make_status(NVME_SCT_GENERIC, NVME_SC_INVALID_FIELD, 1);
        if (bytes > sizeof(page))
            bytes = sizeof(page);
        host = model_xlate(m, cmd->prp1, bytes);
        if (!host)
            return make_status(NVME_SCT_GENERIC, NVME_SC_DATA_TRANSFER_ERROR, 1);
        model_fill_smart(m, page);
        memcpy(host, page, bytes);
        return make_status(NVME_SCT_GENERIC, NVME_SC_SUCCESS, 0);
    }

    case NVME_ADMIN_ABORT:
        *dw0 = 1;
        return make_status(NVME_SCT_GENERIC, NVME_SC_SUCCESS, 0);

    case NVME_ADMIN_NS_MANAGEMENT: {
        uint32_t sel = cmd->cdw10 & 0xfu;
        if (sel == 0) {
            struct nvme_id_ns *req = (struct nvme_id_ns *)model_xlate(m, cmd->prp1, NVME_PAGE_SIZE);
            uint32_t i;
            uint32_t chosen = 0;
            uint32_t lba_bytes;
            uint64_t size_bytes;
            uint64_t offset;

            if (!req)
                return make_status(NVME_SCT_GENERIC, NVME_SC_DATA_TRANSFER_ERROR, 1);
            lba_bytes = (req->flbas & 0xfu) == 1 ? 4096u : 512u;
            size_bytes = req->nsze * lba_bytes;
            if (req->nsze == 0 || size_bytes == 0)
                return make_status(NVME_SCT_GENERIC, NVME_SC_INVALID_FIELD, 1);
            for (i = 1; i <= m->max_namespaces && i <= MODEL_MAX_NAMESPACES; i++) {
                if (!m->ns[i].allocated) {
                    chosen = i;
                    break;
                }
            }
            if (chosen == 0)
                return make_status(NVME_SCT_COMMAND_SPECIFIC, NVME_SC_CS_NS_IDENTIFIER_UNAVAILABLE, 1);
            if (size_bytes > m->capacity_bytes - model_allocated_bytes(m))
                return make_status(NVME_SCT_GENERIC, NVME_SC_CAPACITY_EXCEEDED, 1);
            if (model_ns_place(m, size_bytes, &offset) != 0)
                return make_status(NVME_SCT_GENERIC, NVME_SC_CAPACITY_EXCEEDED, 1);
            m->ns[chosen].allocated = 1;
            m->ns[chosen].attached = 0;
            m->ns[chosen].offset_bytes = offset;
            m->ns[chosen].size_bytes = size_bytes;
            m->ns[chosen].lba_bytes = lba_bytes;
            *dw0 = chosen;
            return make_status(NVME_SCT_GENERIC, NVME_SC_SUCCESS, 0);
        }
        if (sel == 1) {
            uint32_t nsid = cmd->nsid;
            if (nsid == 0 || nsid > MODEL_MAX_NAMESPACES || !m->ns[nsid].allocated)
                return make_status(NVME_SCT_GENERIC, NVME_SC_INVALID_NAMESPACE, 1);
            m->ns[nsid].allocated = 0;
            m->ns[nsid].attached = 0;
            return make_status(NVME_SCT_GENERIC, NVME_SC_SUCCESS, 0);
        }
        return make_status(NVME_SCT_GENERIC, NVME_SC_INVALID_FIELD, 1);
    }

    case NVME_ADMIN_NS_ATTACHMENT: {
        uint32_t sel = cmd->cdw10 & 0xfu;
        uint32_t nsid = cmd->nsid;
        uint16_t *ctrl_list = (uint16_t *)model_xlate(m, cmd->prp1, NVME_PAGE_SIZE);

        if (!ctrl_list)
            return make_status(NVME_SCT_GENERIC, NVME_SC_DATA_TRANSFER_ERROR, 1);
        if (nsid == 0 || nsid > MODEL_MAX_NAMESPACES || !m->ns[nsid].allocated)
            return make_status(NVME_SCT_GENERIC, NVME_SC_INVALID_NAMESPACE, 1);
        if (ctrl_list[0] == 0)
            return make_status(NVME_SCT_GENERIC, NVME_SC_INVALID_FIELD, 1);
        if (sel == 0) {
            if (m->ns[nsid].attached)
                return make_status(NVME_SCT_COMMAND_SPECIFIC, NVME_SC_CS_NS_ALREADY_ATTACHED, 1);
            m->ns[nsid].attached = 1;
            return make_status(NVME_SCT_GENERIC, NVME_SC_SUCCESS, 0);
        }
        if (sel == 1) {
            if (!m->ns[nsid].attached)
                return make_status(NVME_SCT_COMMAND_SPECIFIC, NVME_SC_CS_NS_NOT_ATTACHED, 1);
            m->ns[nsid].attached = 0;
            return make_status(NVME_SCT_GENERIC, NVME_SC_SUCCESS, 0);
        }
        return make_status(NVME_SCT_GENERIC, NVME_SC_INVALID_FIELD, 1);
    }

    case NVME_ADMIN_FORMAT_NVM: {
        uint32_t nsid = cmd->nsid;
        uint32_t lbaf = cmd->cdw10 & 0xfu;
        uint8_t zero[NVME_PAGE_SIZE];
        uint64_t written = 0;

        if (!model_ns_valid(m, nsid))
            return make_status(NVME_SCT_GENERIC, NVME_SC_INVALID_NAMESPACE, 1);
        if (lbaf > 1)
            return make_status(NVME_SCT_GENERIC, NVME_SC_INVALID_FIELD, 1);
        memset(zero, 0, sizeof(zero));
        while (written < m->ns[nsid].size_bytes) {
            uint64_t chunk = m->ns[nsid].size_bytes - written;
            if (chunk > sizeof(zero))
                chunk = sizeof(zero);
            if (pwrite(m->image_fd, zero, chunk, (off_t)(m->ns[nsid].offset_bytes + written)) != (ssize_t)chunk)
                return make_status(NVME_SCT_GENERIC, NVME_SC_INTERNAL_ERROR, 1);
            written += chunk;
        }
        m->ns[nsid].lba_bytes = lbaf == 1 ? 4096u : 512u;
        return make_status(NVME_SCT_GENERIC, NVME_SC_SUCCESS, 0);
    }

    default:
        break;
    }

    return make_status(NVME_SCT_GENERIC, NVME_SC_INVALID_OPCODE, 1);
}

static uint16_t model_exec_io(nvme_model_t *m, const struct nvme_wire_sqe *cmd, uint32_t *dw0)
{
    model_ns_t *ns;
    uint64_t slba;
    uint32_t nlb;
    uint64_t bytes;
    uint64_t media;
    uint16_t status = make_status(NVME_SCT_GENERIC, NVME_SC_SUCCESS, 0);

    *dw0 = 0;

    if (cmd->opcode == NVME_IO_FLUSH) {
        if (!model_ns_valid(m, cmd->nsid) && cmd->nsid != 0xffffffffu)
            return make_status(NVME_SCT_GENERIC, NVME_SC_INVALID_NAMESPACE, 1);
        if (fdatasync(m->image_fd) != 0)
            return make_status(NVME_SCT_GENERIC, NVME_SC_INTERNAL_ERROR, 1);
        return status;
    }

    if (cmd->opcode != NVME_IO_READ && cmd->opcode != NVME_IO_WRITE &&
        cmd->opcode != NVME_IO_WRITE_ZEROES)
        return make_status(NVME_SCT_GENERIC, NVME_SC_INVALID_OPCODE, 1);

    if (!model_ns_valid(m, cmd->nsid))
        return make_status(NVME_SCT_GENERIC, NVME_SC_INVALID_NAMESPACE, 1);

    ns = &m->ns[cmd->nsid];
    slba = ((uint64_t)cmd->cdw11 << 32) | cmd->cdw10;
    nlb = (cmd->cdw12 & 0xffffu) + 1u;
    bytes = (uint64_t)nlb * ns->lba_bytes;

    if (slba + nlb > ns->size_bytes / ns->lba_bytes)
        return make_status(NVME_SCT_GENERIC, NVME_SC_LBA_OUT_OF_RANGE, 1);
    if (bytes > ((uint64_t)NVME_PAGE_SIZE << MODEL_MDTS_SHIFT))
        return make_status(NVME_SCT_GENERIC, NVME_SC_INVALID_FIELD, 1);

    media = ns->offset_bytes + slba * ns->lba_bytes;

    if (cmd->opcode == NVME_IO_WRITE_ZEROES) {
        uint8_t zero[NVME_PAGE_SIZE];
        uint64_t written = 0;
        memset(zero, 0, sizeof(zero));
        while (written < bytes) {
            uint64_t chunk = bytes - written;
            if (chunk > sizeof(zero))
                chunk = sizeof(zero);
            if (pwrite(m->image_fd, zero, chunk, (off_t)(media + written)) != (ssize_t)chunk)
                return make_status(NVME_SCT_GENERIC, NVME_SC_INTERNAL_ERROR, 1);
            written += chunk;
        }
        m->data_units_written += bytes / 512u;
        return status;
    }

    if (model_prp_transfer(m, cmd->prp1, cmd->prp2, bytes,
                           cmd->opcode == NVME_IO_WRITE, media, &status) != 0)
        return status;

    if (cmd->opcode == NVME_IO_WRITE)
        m->data_units_written += bytes / 512u;
    else
        m->data_units_read += bytes / 512u;

    return status;
}

static void model_delay(nvme_model_t *m)
{
    struct timespec ts;

    if (m->latency_us == 0)
        return;
    ts.tv_sec = m->latency_us / 1000000u;
    ts.tv_nsec = (long)(m->latency_us % 1000000u) * 1000L;
    nanosleep(&ts, NULL);
}

static int model_service_sq(nvme_model_t *m, uint16_t qid)
{
    model_sq_t *sq = &m->sq[qid];
    uint32_t tail;
    int serviced = 0;

    if (!sq->active)
        return 0;

    tail = sq_doorbell(m, qid) % sq->depth;
    while (sq->head != tail) {
        struct nvme_wire_sqe cmd;
        struct nvme_wire_sqe *slot;
        uint32_t dw0 = 0;
        uint16_t status;

        slot = (struct nvme_wire_sqe *)model_xlate(m,
                    sq->base_iova + (uint64_t)sq->head * NVME_SQE_BYTES, NVME_SQE_BYTES);
        if (!slot) {
            reg_store(m, NVME_REG_CSTS, reg_load(m, NVME_REG_CSTS) | NVME_CSTS_CFS);
            return serviced;
        }
        memcpy(&cmd, slot, sizeof(cmd));
        __atomic_thread_fence(__ATOMIC_ACQUIRE);

        sq->head = (uint16_t)((sq->head + 1u) % sq->depth);

        model_delay(m);
        status = qid == 0 ? model_exec_admin(m, &cmd, &dw0) : model_exec_io(m, &cmd, &dw0);
        __atomic_fetch_add(&m->commands_executed, 1, __ATOMIC_RELAXED);

        model_post_completion(m, qid == 0 ? 0 : sq->cqid, qid, sq->head, cmd.cid, dw0, status);
        serviced = 1;
    }
    return serviced;
}

static void model_enable(nvme_model_t *m)
{
    uint32_t aqa = reg_load(m, NVME_REG_AQA);
    uint16_t asqs = (uint16_t)((aqa & 0xfffu) + 1u);
    uint16_t acqs = (uint16_t)(((aqa >> 16) & 0xfffu) + 1u);

    memset(m->sq, 0, sizeof(m->sq));
    memset(m->cq, 0, sizeof(m->cq));

    m->sq[0].active = 1;
    m->sq[0].depth = asqs;
    m->sq[0].head = 0;
    m->sq[0].cqid = 0;
    m->sq[0].base_iova = reg_load64(m, NVME_REG_ASQ);

    m->cq[0].active = 1;
    m->cq[0].depth = acqs;
    m->cq[0].tail = 0;
    m->cq[0].phase = 1;
    m->cq[0].irq_enabled = 1;
    m->cq[0].irq_vector = 0;
    m->cq[0].base_iova = reg_load64(m, NVME_REG_ACQ);

    m->enabled = 1;
    reg_store(m, NVME_REG_CSTS, NVME_CSTS_RDY);
}

static void model_disable(nvme_model_t *m)
{
    memset(m->sq, 0, sizeof(m->sq));
    memset(m->cq, 0, sizeof(m->cq));
    memset(m->bar + NVME_REG_DOORBELL_BASE, 0, MODEL_BAR_SIZE - NVME_REG_DOORBELL_BASE);
    m->enabled = 0;
    reg_store(m, NVME_REG_CSTS, 0);
}

static void *model_thread(void *arg)
{
    nvme_model_t *m = (nvme_model_t *)arg;
    unsigned idle = 0;

    while (!__atomic_load_n(&m->stop, __ATOMIC_ACQUIRE)) {
        uint32_t cc = reg_load(m, NVME_REG_CC);
        int worked = 0;

        if ((cc & NVME_CC_EN) && !m->enabled)
            model_enable(m);
        else if (!(cc & NVME_CC_EN) && m->enabled)
            model_disable(m);

        if (m->enabled) {
            uint16_t qid;
            for (qid = 0; qid < MODEL_MAX_QUEUES; qid++)
                worked |= model_service_sq(m, qid);
        }

        if (worked) {
            idle = 0;
        } else if (++idle < 512) {
            sched_yield();
        } else {
            struct timespec ts = { 0, 50000 };
            nanosleep(&ts, NULL);
        }
    }
    return NULL;
}

static void model_init_config_space(nvme_model_t *m)
{
    uint8_t *cfg = m->config;

    memset(cfg, 0, MODEL_CONFIG_SIZE);
    cfg[0x00] = 0x36;
    cfg[0x01] = 0x1b;
    cfg[0x02] = 0x10;
    cfg[0x03] = 0x00;
    cfg[0x04] = 0x00;
    cfg[0x05] = 0x00;
    cfg[0x06] = 0x10;
    cfg[0x07] = 0x00;
    cfg[0x08] = 0x02;
    cfg[0x09] = 0x02;
    cfg[0x0a] = 0x08;
    cfg[0x0b] = 0x01;
    cfg[0x0e] = 0x00;
    cfg[0x10] = 0x04;
    cfg[0x11] = 0x00;
    cfg[0x12] = 0x00;
    cfg[0x13] = 0x00;
    cfg[0x2c] = 0xf4;
    cfg[0x2d] = 0x1a;
    cfg[0x2e] = 0x00;
    cfg[0x2f] = 0x11;
    cfg[0x34] = 0x40;
    cfg[0x3d] = 0x01;
    cfg[0x40] = 0x11;
    cfg[0x41] = 0x00;
    cfg[0x42] = (uint8_t)(MODEL_MAX_MSIX - 1);
    cfg[0x43] = 0x80;
}

uint8_t *nvme_model_config_space(nvme_model_t *m)
{
    return m->config;
}

size_t nvme_model_config_size(nvme_model_t *m)
{
    (void)m;
    return MODEL_CONFIG_SIZE;
}

void *nvme_model_bar(nvme_model_t *m)
{
    return m->bar;
}

size_t nvme_model_bar_size(nvme_model_t *m)
{
    (void)m;
    return MODEL_BAR_SIZE;
}

void nvme_model_device_reset(nvme_model_t *m)
{
    reg_store(m, NVME_REG_CC, 0);
    while (m->enabled) {
        struct timespec ts = { 0, 100000 };
        nanosleep(&ts, NULL);
    }
    memset(m->bar + NVME_REG_DOORBELL_BASE, 0, MODEL_BAR_SIZE - NVME_REG_DOORBELL_BASE);
}

nvme_model_t *nvme_model_create(const lt_mock_config_t *cfg, char *errbuf, size_t errlen)
{
    nvme_model_t *m;
    uint64_t cap;
    uint64_t primary_bytes;

    m = calloc(1, sizeof(*m));
    if (!m) {
        snprintf(errbuf, errlen, "Out of memory");
        return NULL;
    }

    m->bar = aligned_alloc(NVME_PAGE_SIZE, MODEL_BAR_SIZE);
    if (!m->bar) {
        snprintf(errbuf, errlen, "Cannot allocate the register window");
        free(m);
        return NULL;
    }
    memset(m->bar, 0, MODEL_BAR_SIZE);

    m->capacity_bytes = cfg->capacity_bytes;
    m->lba_bytes = cfg->lba_bytes ? cfg->lba_bytes : 512u;
    m->max_namespaces = cfg->max_namespaces ? cfg->max_namespaces : 4u;
    if (m->max_namespaces > MODEL_MAX_NAMESPACES)
        m->max_namespaces = MODEL_MAX_NAMESPACES;
    m->latency_us = cfg->queue_latency_us;
    snprintf(m->serial, sizeof(m->serial), "%s", cfg->serial ? cfg->serial : "DEADBEEF");
    snprintf(m->model_name, sizeof(m->model_name), "%s", cfg->model ? cfg->model : "lantern mock nvme");

    m->image_fd = open(cfg->image_path, O_RDWR | O_CREAT, 0600);
    if (m->image_fd < 0) {
        snprintf(errbuf, errlen, "Cannot open backing image %s: %s", cfg->image_path, strerror(errno));
        free(m->bar);
        free(m);
        return NULL;
    }
    if (ftruncate(m->image_fd, (off_t)m->capacity_bytes) != 0) {
        snprintf(errbuf, errlen, "Cannot size the backing image: %s", strerror(errno));
        close(m->image_fd);
        free(m->bar);
        free(m);
        return NULL;
    }

    primary_bytes = m->max_namespaces > 1 ? m->capacity_bytes / 2u : m->capacity_bytes;
    primary_bytes -= primary_bytes % m->lba_bytes;
    m->ns[1].allocated = 1;
    m->ns[1].attached = 1;
    m->ns[1].offset_bytes = 0;
    m->ns[1].size_bytes = primary_bytes;
    m->ns[1].lba_bytes = m->lba_bytes;

    pthread_mutex_init(&m->map_lock, NULL);
    memset(m->msix_fds, 0xff, sizeof(m->msix_fds));

    model_init_config_space(m);

    cap = (uint64_t)(1024u - 1u);
    cap |= (uint64_t)1u << 16;
    cap |= (uint64_t)2u << 24;
    cap |= (uint64_t)0u << 32;
    cap |= (uint64_t)1u << 37;
    cap |= (uint64_t)0u << 48;
    cap |= (uint64_t)4u << 52;
    reg_store64(m, NVME_REG_CAP, cap);
    reg_store(m, NVME_REG_VS, 0x00010400u);
    reg_store(m, NVME_REG_CSTS, 0);

    if (pthread_create(&m->thread, NULL, model_thread, m) != 0) {
        snprintf(errbuf, errlen, "Cannot start the controller thread");
        close(m->image_fd);
        pthread_mutex_destroy(&m->map_lock);
        free(m->bar);
        free(m);
        return NULL;
    }
    return m;
}

void nvme_model_destroy(nvme_model_t *m)
{
    if (!m)
        return;
    __atomic_store_n(&m->stop, 1, __ATOMIC_RELEASE);
    pthread_join(m->thread, NULL);
    if (m->image_fd >= 0)
        close(m->image_fd);
    pthread_mutex_destroy(&m->map_lock);
    free(m->bar);
    free(m);
}
