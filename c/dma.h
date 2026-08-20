#ifndef LANTERN_DMA_H
#define LANTERN_DMA_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NVME_DMA_OK               0
#define NVME_DMA_ERR_INVALID     -1
#define NVME_DMA_ERR_BACKEND     -2
#define NVME_DMA_ERR_NO_DEVICE   -3
#define NVME_DMA_ERR_TIMEOUT     -4
#define NVME_DMA_ERR_NOMEM       -5
#define NVME_DMA_ERR_CONTROLLER  -6
#define NVME_DMA_ERR_QUEUE_FULL  -7
#define NVME_DMA_ERR_STATE       -8
#define NVME_DMA_ERR_RANGE       -9

#define NVME_DMA_BACKEND_VFIO 0
#define NVME_DMA_BACKEND_MOCK 1

#define NVME_DMA_MAX_QUEUES 17

typedef struct nvme_dev nvme_dev_t;
typedef struct nvme_dma nvme_dma_t;

typedef struct {
    int         backend;
    uint16_t    admin_queue_depth;
    int         enable_msix;
    const char *mock_image_path;
    uint64_t    mock_capacity_bytes;
    uint32_t    mock_lba_bytes;
    uint32_t    mock_namespaces;
    uint32_t    mock_latency_us;
    const char *mock_serial;
    const char *mock_model;
} nvme_dev_config_t;

typedef struct {
    uint32_t dw[16];
} nvme_sqe_t;

typedef struct {
    uint32_t dw0;
    uint16_t sq_head;
    uint16_t sq_id;
    uint16_t cid;
    uint16_t status;
} nvme_cqe_t;

typedef struct {
    uint64_t cap;
    uint32_t vs;
    uint32_t cc;
    uint32_t csts;
    uint32_t aqa;
    uint64_t asq;
    uint64_t acq;
    uint32_t max_queue_entries;
    uint32_t doorbell_stride;
    uint32_t timeout_ms;
    uint32_t page_size;
    uint32_t max_transfer_bytes;
} nvme_dev_state_t;

typedef struct {
    int      active;
    uint16_t qid;
    uint16_t depth;
    uint16_t sq_tail;
    uint16_t cq_head;
    uint8_t  cq_phase;
    uint64_t sq_iova;
    uint64_t cq_iova;
    uint64_t submitted;
    uint64_t completed;
} nvme_queue_state_t;

void nvme_dev_config_defaults(nvme_dev_config_t *config);

int  nvme_dev_open(const char *bdf, const nvme_dev_config_t *config, nvme_dev_t **out);
int  nvme_dev_reset(nvme_dev_t *dev);
int  nvme_dev_close(nvme_dev_t *dev);

const char *nvme_dev_backend_name(const nvme_dev_t *dev);
const char *nvme_dev_bdf(const nvme_dev_t *dev);
const char *nvme_dev_error(const nvme_dev_t *dev);
const char *nvme_strerror(int code);

int nvme_dev_state(const nvme_dev_t *dev, nvme_dev_state_t *state);
int nvme_queue_state(const nvme_dev_t *dev, uint16_t qid, nvme_queue_state_t *state);

int      nvme_dma_alloc(nvme_dev_t *dev, size_t size, nvme_dma_t **out);
void     nvme_dma_free(nvme_dev_t *dev, nvme_dma_t *buffer);
uint64_t nvme_dma_iova(const nvme_dma_t *buffer);
size_t   nvme_dma_size(const nvme_dma_t *buffer);
int      nvme_dma_write(nvme_dma_t *buffer, size_t offset, const void *src, size_t len);
int      nvme_dma_read(const nvme_dma_t *buffer, size_t offset, void *dst, size_t len);
int      nvme_dma_fill(nvme_dma_t *buffer, size_t offset, int byte, size_t len);
int      nvme_dma_pattern(nvme_dma_t *buffer, size_t offset, size_t len, uint64_t seed);
int      nvme_dma_equal(const nvme_dma_t *a, const nvme_dma_t *b, size_t offset, size_t len);
int         nvme_dma_id(const nvme_dev_t *dev, const nvme_dma_t *buffer);
nvme_dma_t *nvme_dma_handle(nvme_dev_t *dev, int id);

void     nvme_sqe_init(nvme_sqe_t *sqe, uint8_t opcode, uint16_t cid, uint32_t nsid);
void     nvme_sqe_set_prp(nvme_sqe_t *sqe, uint64_t prp1, uint64_t prp2);
void     nvme_sqe_set_cdw(nvme_sqe_t *sqe, unsigned index, uint32_t value);
uint32_t nvme_sqe_get_cdw(const nvme_sqe_t *sqe, unsigned index);
uint16_t nvme_sqe_cid(const nvme_sqe_t *sqe);

int nvme_prp_build(nvme_dev_t *dev, uint16_t qid, uint16_t cid, const nvme_dma_t *buffer,
                   size_t offset, size_t len, nvme_sqe_t *sqe);

int nvme_admin_submit(nvme_dev_t *dev, const nvme_sqe_t *sqe);
int nvme_admin_poll(nvme_dev_t *dev, nvme_cqe_t *cqe, int timeout_ms);
int nvme_io_submit(nvme_dev_t *dev, uint16_t qid, const nvme_sqe_t *sqe);
int nvme_io_poll(nvme_dev_t *dev, uint16_t qid, nvme_cqe_t *cqe, int timeout_ms);

int nvme_io_queue_alloc(nvme_dev_t *dev, uint16_t qid, uint16_t depth);
int nvme_io_queue_addresses(const nvme_dev_t *dev, uint16_t qid, uint64_t *sq_iova, uint64_t *cq_iova);
int nvme_io_queue_activate(nvme_dev_t *dev, uint16_t qid);
int nvme_io_queue_free(nvme_dev_t *dev, uint16_t qid);

int nvme_irq_enable(nvme_dev_t *dev, unsigned vectors);
int nvme_irq_disable(nvme_dev_t *dev);
int nvme_irq_fd(const nvme_dev_t *dev, unsigned vector);
int nvme_irq_wait(const nvme_dev_t *dev, unsigned vector, int timeout_ms);
unsigned nvme_irq_count(const nvme_dev_t *dev);

#ifdef __cplusplus
}
#endif

#endif
