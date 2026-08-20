#define _GNU_SOURCE
#include "dma.h"
#include "spec.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <caml/memory.h>
#include <caml/mlvalues.h>
#include <caml/threads.h>

int lantern_selftest_run(const char *bdf, const char *image, int backend);

static int failures;
static int checks;

static void check(int condition, const char *what, const char *detail)
{
    checks++;
    if (condition) {
        printf("  [ ok ] %s%s%s\n", what, detail ? " :: " : "", detail ? detail : "");
    } else {
        failures++;
        printf("  [FAIL] %s%s%s\n", what, detail ? " :: " : "", detail ? detail : "");
    }
}

static void dump_registers(nvme_dev_t *dev, const char *stage)
{
    nvme_dev_state_t state;

    if (nvme_dev_state(dev, &state) != NVME_DMA_OK) {
        printf("  Registers[%s] unavailable\n", stage);
        return;
    }
    printf("  Registers[%s] CAP=0x%016llx VS=%u.%u.%u CC=0x%08x CSTS=0x%08x AQA=0x%08x\n",
           stage, (unsigned long long)state.cap,
           (state.vs >> 16) & 0xffffu, (state.vs >> 8) & 0xffu, state.vs & 0xffu,
           state.cc, state.csts, state.aqa);
    printf("  Registers[%s] ASQ=0x%016llx ACQ=0x%016llx MQES=%u DSTRD=%u TO=%ums MDTS=%u bytes\n",
           stage, (unsigned long long)state.asq, (unsigned long long)state.acq,
           state.max_queue_entries, state.doorbell_stride, state.timeout_ms,
           state.max_transfer_bytes);
}

static void dump_queue(nvme_dev_t *dev, uint16_t qid, const char *stage)
{
    nvme_queue_state_t q;

    if (nvme_queue_state(dev, qid, &q) != NVME_DMA_OK)
        return;
    printf("  Queue[%u %s] active=%d depth=%u SQ tail=%u CQ head=%u phase=%u "
           "SQ IOVA=0x%llx CQ IOVA=0x%llx submitted=%llu completed=%llu\n",
           qid, stage, q.active, q.depth, q.sq_tail, q.cq_head, q.cq_phase,
           (unsigned long long)q.sq_iova, (unsigned long long)q.cq_iova,
           (unsigned long long)q.submitted, (unsigned long long)q.completed);
}

static int admin_roundtrip(nvme_dev_t *dev, const nvme_sqe_t *sqe, nvme_cqe_t *cqe)
{
    int rc = nvme_admin_submit(dev, sqe);

    if (rc != NVME_DMA_OK)
        return rc;
    rc = nvme_admin_poll(dev, cqe, 5000);
    if (rc == 0)
        return NVME_DMA_ERR_TIMEOUT;
    return rc < 0 ? rc : NVME_DMA_OK;
}

static int io_roundtrip(nvme_dev_t *dev, uint16_t qid, const nvme_sqe_t *sqe, nvme_cqe_t *cqe)
{
    int rc = nvme_io_submit(dev, qid, sqe);

    if (rc != NVME_DMA_OK)
        return rc;
    rc = nvme_io_poll(dev, qid, cqe, 5000);
    if (rc == 0)
        return NVME_DMA_ERR_TIMEOUT;
    return rc < 0 ? rc : NVME_DMA_OK;
}

static void trim(char *dst, const char *src, size_t len)
{
    size_t end = len;

    while (end > 0 && (src[end - 1] == ' ' || src[end - 1] == '\0'))
        end--;
    memcpy(dst, src, end);
    dst[end] = '\0';
}

int lantern_selftest_run(const char *bdf, const char *image, int backend)
{
    const int mock = backend == NVME_DMA_BACKEND_MOCK;
    nvme_dev_config_t config;
    nvme_dev_t *dev = NULL;
    nvme_dma_t *identify_buffer = NULL;
    nvme_dma_t *write_buffer = NULL;
    nvme_dma_t *read_buffer = NULL;
    nvme_sqe_t sqe;
    nvme_cqe_t cqe;
    struct nvme_id_ctrl id_ctrl;
    struct nvme_id_ns id_ns;
    char text[64];
    uint64_t namespace_blocks;
    uint32_t lba_bytes;
    int rc;

    nvme_dev_config_defaults(&config);
    config.backend = backend;
    config.mock_image_path = image;
    config.mock_capacity_bytes = 64ull * 1024ull * 1024ull;
    config.mock_lba_bytes = 512;
    config.mock_namespaces = 4;
    config.admin_queue_depth = 32;

    printf("Phase 1: VFIO attach and controller enable\n");
    rc = nvme_dev_open(bdf, &config, &dev);
    check(rc == NVME_DMA_OK, "nvme_dev_open", nvme_strerror(rc));
    if (rc != NVME_DMA_OK)
        return 1;
    printf("  Backend=%s BDF=%s\n", nvme_dev_backend_name(dev), nvme_dev_bdf(dev));
    dump_registers(dev, "enabled");
    dump_queue(dev, 0, "admin");

    {
        nvme_dev_state_t state;
        nvme_dev_state(dev, &state);
        check((state.csts & NVME_CSTS_RDY) != 0, "Controller reports ready", NULL);
        check((state.cc & NVME_CC_EN) != 0, "CC.EN set", NULL);
        check(((state.cc >> NVME_CC_IOSQES_SHIFT) & 0xfu) == 6, "CC.IOSQES is 64 byte entries", NULL);
        check(((state.cc >> NVME_CC_IOCQES_SHIFT) & 0xfu) == 4, "CC.IOCQES is 16 byte entries", NULL);
        check(state.asq != 0 && state.acq != 0, "Admin queue base addresses programmed", NULL);
    }

    rc = nvme_dma_alloc(dev, NVME_PAGE_SIZE, &identify_buffer);
    check(rc == NVME_DMA_OK, "DMA allocation for the identify buffer", nvme_strerror(rc));

    printf("Phase 2: admin commands\n");
    nvme_sqe_init(&sqe, NVME_ADMIN_IDENTIFY, 1, 0);
    nvme_sqe_set_prp(&sqe, nvme_dma_iova(identify_buffer), 0);
    nvme_sqe_set_cdw(&sqe, 10, NVME_IDENTIFY_CNS_CONTROLLER);
    rc = admin_roundtrip(dev, &sqe, &cqe);
    check(rc == NVME_DMA_OK && cqe.status == 0, "Identify Controller", nvme_strerror(rc));
    dump_queue(dev, 0, "after identify");

    nvme_dma_read(identify_buffer, 0, &id_ctrl, sizeof(id_ctrl));
    trim(text, id_ctrl.mn, sizeof(id_ctrl.mn));
    printf("  Model      : %s\n", text);
    trim(text, id_ctrl.sn, sizeof(id_ctrl.sn));
    printf("  Serial     : %s\n", text);
    if (mock)
        check(strcmp(text, "DEADBEEF") == 0, "Serial matches the configured device", text);
    else
        check(strlen(text) > 0, "Controller reports a serial number", text);
    trim(text, id_ctrl.fr, sizeof(id_ctrl.fr));
    printf("  Firmware   : %s\n", text);
    printf("  VID        : 0x%04x  CNTLID: %u  version: 0x%08x\n", id_ctrl.vid, id_ctrl.cntlid, id_ctrl.ver);
    printf("  MDTS       : %u (%u bytes)  NN: %u  SQES: 0x%02x CQES: 0x%02x\n",
           id_ctrl.mdts, NVME_PAGE_SIZE << id_ctrl.mdts, id_ctrl.nn, id_ctrl.sqes, id_ctrl.cqes);
    check(id_ctrl.nn >= 1, "Controller reports at least one namespace", NULL);
    check(id_ctrl.sqes == 0x66 && id_ctrl.cqes == 0x44, "Queue entry sizes advertised", NULL);

    nvme_sqe_init(&sqe, NVME_ADMIN_IDENTIFY, 2, 1);
    nvme_sqe_set_prp(&sqe, nvme_dma_iova(identify_buffer), 0);
    nvme_sqe_set_cdw(&sqe, 10, NVME_IDENTIFY_CNS_NAMESPACE);
    rc = admin_roundtrip(dev, &sqe, &cqe);
    check(rc == NVME_DMA_OK && cqe.status == 0, "Identify Namespace 1", nvme_strerror(rc));

    nvme_dma_read(identify_buffer, 0, &id_ns, sizeof(id_ns));
    lba_bytes = 1u << id_ns.lbaf[id_ns.flbas & 0xfu].lbads;
    namespace_blocks = id_ns.nsze;
    printf("  NSZE       : %llu blocks  NCAP: %llu  NUSE: %llu\n",
           (unsigned long long)id_ns.nsze, (unsigned long long)id_ns.ncap,
           (unsigned long long)id_ns.nuse);
    printf("  LBA format : index %u, %u bytes per block, %llu bytes total\n",
           id_ns.flbas & 0xfu, lba_bytes, (unsigned long long)(namespace_blocks * lba_bytes));
    check(lba_bytes >= 512, "Namespace reports a usable block size", NULL);
    if (mock)
        check(namespace_blocks * lba_bytes == config.mock_capacity_bytes / 2,
              "Namespace size matches the provisioned capacity", NULL);
    else
        check(namespace_blocks > 0, "Namespace reports a non empty block count", NULL);

    nvme_sqe_init(&sqe, NVME_ADMIN_IDENTIFY, 3, 0);
    nvme_sqe_set_prp(&sqe, nvme_dma_iova(identify_buffer), 0);
    nvme_sqe_set_cdw(&sqe, 10, NVME_IDENTIFY_CNS_NS_LIST);
    rc = admin_roundtrip(dev, &sqe, &cqe);
    check(rc == NVME_DMA_OK && cqe.status == 0, "Identify Active Namespace List", nvme_strerror(rc));
    {
        uint32_t list[4];
        nvme_dma_read(identify_buffer, 0, list, sizeof(list));
        printf("  Active NSID list: %u %u %u %u\n", list[0], list[1], list[2], list[3]);
        check(list[0] == 1, "Namespace 1 present in the active list", NULL);
    }

    nvme_sqe_init(&sqe, NVME_ADMIN_SET_FEATURES, 4, 0);
    nvme_sqe_set_cdw(&sqe, 10, NVME_FEATURE_NUM_QUEUES);
    nvme_sqe_set_cdw(&sqe, 11, (3u << 16) | 3u);
    rc = admin_roundtrip(dev, &sqe, &cqe);
    check(rc == NVME_DMA_OK && cqe.status == 0, "Set Features, Number of Queues", nvme_strerror(rc));
    printf("  Granted    : %u submission queues, %u completion queues\n",
           (cqe.dw0 & 0xffffu) + 1u, ((cqe.dw0 >> 16) & 0xffffu) + 1u);

    printf("Phase 3: I/O queue pair and block transfers\n");
    rc = nvme_io_queue_alloc(dev, 1, 64);
    check(rc == NVME_DMA_OK, "I/O queue memory allocation", nvme_strerror(rc));
    {
        uint64_t sq_iova = 0;
        uint64_t cq_iova = 0;
        nvme_io_queue_addresses(dev, 1, &sq_iova, &cq_iova);
        printf("  Queue 1 SQ IOVA=0x%llx CQ IOVA=0x%llx\n",
               (unsigned long long)sq_iova, (unsigned long long)cq_iova);

        nvme_sqe_init(&sqe, NVME_ADMIN_CREATE_CQ, 5, 0);
        nvme_sqe_set_prp(&sqe, cq_iova, 0);
        nvme_sqe_set_cdw(&sqe, 10, (63u << 16) | 1u);
        nvme_sqe_set_cdw(&sqe, 11, 1u);
        rc = admin_roundtrip(dev, &sqe, &cqe);
        check(rc == NVME_DMA_OK && cqe.status == 0, "Create I/O Completion Queue", nvme_strerror(rc));

        nvme_sqe_init(&sqe, NVME_ADMIN_CREATE_SQ, 6, 0);
        nvme_sqe_set_prp(&sqe, sq_iova, 0);
        nvme_sqe_set_cdw(&sqe, 10, (63u << 16) | 1u);
        nvme_sqe_set_cdw(&sqe, 11, (1u << 16) | 1u);
        rc = admin_roundtrip(dev, &sqe, &cqe);
        check(rc == NVME_DMA_OK && cqe.status == 0, "Create I/O Submission Queue", nvme_strerror(rc));
    }
    rc = nvme_io_queue_activate(dev, 1);
    check(rc == NVME_DMA_OK, "Activate I/O queue 1", nvme_strerror(rc));
    dump_queue(dev, 1, "created");

    rc = nvme_dma_alloc(dev, 128u * 1024u, &write_buffer);
    check(rc == NVME_DMA_OK, "DMA allocation for the write buffer", nvme_strerror(rc));
    rc = nvme_dma_alloc(dev, 128u * 1024u, &read_buffer);
    check(rc == NVME_DMA_OK, "DMA allocation for the read buffer", nvme_strerror(rc));

    {
        const uint64_t slba = 0x200;
        const uint32_t blocks = 8;
        const size_t bytes = blocks * lba_bytes;

        nvme_dma_pattern(write_buffer, 0, bytes, 0xcafef00dull);
        nvme_dma_fill(read_buffer, 0, 0, bytes);

        nvme_sqe_init(&sqe, NVME_IO_WRITE, 1, 1);
        nvme_prp_build(dev, 1, 1, write_buffer, 0, bytes, &sqe);
        nvme_sqe_set_cdw(&sqe, 10, (uint32_t)slba);
        nvme_sqe_set_cdw(&sqe, 11, (uint32_t)(slba >> 32));
        nvme_sqe_set_cdw(&sqe, 12, blocks - 1u);
        rc = io_roundtrip(dev, 1, &sqe, &cqe);
        check(rc == NVME_DMA_OK && cqe.status == 0, "Write 8 blocks", nvme_strerror(rc));

        nvme_sqe_init(&sqe, NVME_IO_READ, 2, 1);
        nvme_prp_build(dev, 1, 2, read_buffer, 0, bytes, &sqe);
        nvme_sqe_set_cdw(&sqe, 10, (uint32_t)slba);
        nvme_sqe_set_cdw(&sqe, 11, (uint32_t)(slba >> 32));
        nvme_sqe_set_cdw(&sqe, 12, blocks - 1u);
        rc = io_roundtrip(dev, 1, &sqe, &cqe);
        check(rc == NVME_DMA_OK && cqe.status == 0, "Read 8 blocks back", nvme_strerror(rc));

        check(nvme_dma_equal(write_buffer, read_buffer, 0, bytes) == 1,
              "Round trip data matches", NULL);
        dump_queue(dev, 1, "after round trip");
    }

    {
        const uint64_t slba = 0x400;
        const uint32_t blocks = 256;
        const size_t bytes = blocks * lba_bytes;

        nvme_dma_pattern(write_buffer, 0, bytes, 0x1234567890abcdefull);
        nvme_dma_fill(read_buffer, 0, 0xa5, bytes);

        nvme_sqe_init(&sqe, NVME_IO_WRITE, 3, 1);
        rc = nvme_prp_build(dev, 1, 3, write_buffer, 0, bytes, &sqe);
        check(rc == NVME_DMA_OK, "PRP list construction for a 128 KiB transfer", nvme_strerror(rc));
        check(nvme_sqe_get_cdw(&sqe, 8) != 0 || nvme_sqe_get_cdw(&sqe, 9) != 0,
              "PRP2 points at a PRP list", NULL);
        nvme_sqe_set_cdw(&sqe, 10, (uint32_t)slba);
        nvme_sqe_set_cdw(&sqe, 12, blocks - 1u);
        rc = io_roundtrip(dev, 1, &sqe, &cqe);
        check(rc == NVME_DMA_OK && cqe.status == 0, "Write 128 KiB via a PRP list", nvme_strerror(rc));

        nvme_sqe_init(&sqe, NVME_IO_READ, 4, 1);
        nvme_prp_build(dev, 1, 4, read_buffer, 0, bytes, &sqe);
        nvme_sqe_set_cdw(&sqe, 10, (uint32_t)slba);
        nvme_sqe_set_cdw(&sqe, 12, blocks - 1u);
        rc = io_roundtrip(dev, 1, &sqe, &cqe);
        check(rc == NVME_DMA_OK && cqe.status == 0, "Read 128 KiB via a PRP list", nvme_strerror(rc));
        check(nvme_dma_equal(write_buffer, read_buffer, 0, bytes) == 1,
              "Large round trip data matches", NULL);
    }

    {
        nvme_sqe_init(&sqe, NVME_IO_READ, 5, 1);
        nvme_prp_build(dev, 1, 5, read_buffer, 0, lba_bytes, &sqe);
        nvme_sqe_set_cdw(&sqe, 10, (uint32_t)(namespace_blocks + 16u));
        nvme_sqe_set_cdw(&sqe, 12, 0);
        rc = io_roundtrip(dev, 1, &sqe, &cqe);
        printf("  Status word for out of range read: SCT=%u SC=0x%02x DNR=%u\n",
               (cqe.status >> 8) & 0x7u, cqe.status & 0xffu, (cqe.status >> 14) & 1u);
        check(rc == NVME_DMA_OK && (cqe.status & 0xffu) == NVME_SC_LBA_OUT_OF_RANGE,
              "Read past the end of the namespace rejected", NULL);
    }

    {
        nvme_sqe_init(&sqe, 0x7f, 6, 1);
        nvme_sqe_set_prp(&sqe, nvme_dma_iova(read_buffer), 0);
        rc = io_roundtrip(dev, 1, &sqe, &cqe);
        check(rc == NVME_DMA_OK && (cqe.status & 0xffu) == NVME_SC_INVALID_OPCODE,
              "Unknown I/O opcode rejected", NULL);
    }

    {
        nvme_sqe_init(&sqe, NVME_IO_FLUSH, 7, 1);
        rc = io_roundtrip(dev, 1, &sqe, &cqe);
        check(rc == NVME_DMA_OK && cqe.status == 0, "Flush", nvme_strerror(rc));
    }

    printf("Phase 1 revisited: controller reset\n");
    dump_registers(dev, "before reset");
    rc = nvme_dev_reset(dev);
    check(rc == NVME_DMA_OK, "nvme_dev_reset", nvme_strerror(rc));
    dump_registers(dev, "after reset");
    dump_queue(dev, 0, "after reset");
    {
        nvme_dev_state_t state;
        nvme_dev_state(dev, &state);
        check((state.csts & NVME_CSTS_RDY) != 0, "Controller ready again after reset", NULL);
    }

    nvme_sqe_init(&sqe, NVME_ADMIN_IDENTIFY, 9, 0);
    nvme_sqe_set_prp(&sqe, nvme_dma_iova(identify_buffer), 0);
    nvme_sqe_set_cdw(&sqe, 10, NVME_IDENTIFY_CNS_CONTROLLER);
    rc = admin_roundtrip(dev, &sqe, &cqe);
    check(rc == NVME_DMA_OK && cqe.status == 0, "Admin queue works after reset", nvme_strerror(rc));

    nvme_dma_free(dev, identify_buffer);
    nvme_dma_free(dev, write_buffer);
    nvme_dma_free(dev, read_buffer);
    rc = nvme_dev_close(dev);
    check(rc == NVME_DMA_OK, "nvme_dev_close", nvme_strerror(rc));

    printf("\n%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}

CAMLprim value lantern_selftest_entry(value vbdf, value vimage, value vbackend)
{
    CAMLparam3(vbdf, vimage, vbackend);
    char bdf[32];
    char image[512];
    int rc;

    snprintf(bdf, sizeof(bdf), "%s", String_val(vbdf));
    snprintf(image, sizeof(image), "%s", String_val(vimage));
    caml_release_runtime_system();
    rc = lantern_selftest_run(bdf, image, Int_val(vbackend));
    caml_acquire_runtime_system();
    CAMLreturn(Val_int(rc));
}
