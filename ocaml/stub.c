#define _GNU_SOURCE
#include "dma.h"

#include <stdio.h>

#include <caml/alloc.h>
#include <caml/callback.h>
#include <caml/custom.h>
#include <caml/fail.h>
#include <caml/memory.h>
#include <caml/mlvalues.h>
#include <caml/threads.h>

#include <stdlib.h>
#include <string.h>

typedef struct {
    nvme_dev_t *dev;
    int         closed;
    char       *image_path;
    char       *serial;
    char       *model;
} lantern_handle_t;

#define Handle_val(v) (*((lantern_handle_t **)Data_custom_val(v)))

static void handle_release(lantern_handle_t *handle)
{
    if (!handle)
        return;
    if (!handle->closed && handle->dev) {
        nvme_dev_close(handle->dev);
        handle->closed = 1;
        handle->dev = NULL;
    }
    free(handle->image_path);
    free(handle->serial);
    free(handle->model);
    handle->image_path = NULL;
    handle->serial = NULL;
    handle->model = NULL;
}

static void lantern_handle_finalize(value v)
{
    lantern_handle_t *handle = Handle_val(v);

    handle_release(handle);
    free(handle);
    Handle_val(v) = NULL;
}

static struct custom_operations lantern_handle_ops = {
    "lantern.device",
    lantern_handle_finalize,
    custom_compare_default,
    custom_hash_default,
    custom_serialize_default,
    custom_deserialize_default,
    custom_compare_ext_default,
    custom_fixed_length_default
};

static value result_ok(value payload)
{
    CAMLparam1(payload);
    CAMLlocal1(block);

    block = caml_alloc(1, 0);
    Store_field(block, 0, payload);
    CAMLreturn(block);
}

static value result_error(const char *message)
{
    CAMLparam0();
    CAMLlocal2(block, text);

    text = caml_copy_string(message ? message : "Unknown error");
    block = caml_alloc(1, 1);
    Store_field(block, 0, text);
    CAMLreturn(block);
}

static char *dup_string(value v)
{
    const char *source = String_val(v);
    size_t len = caml_string_length(v);
    char *out = malloc(len + 1);

    if (!out)
        return NULL;
    memcpy(out, source, len);
    out[len] = '\0';
    return out;
}

CAMLprim value lantern_dev_open(value vbdf, value vconfig)
{
    CAMLparam2(vbdf, vconfig);
    CAMLlocal2(custom, payload);
    nvme_dev_config_t config;
    lantern_handle_t *handle;
    nvme_dev_t *dev = NULL;
    char bdf[32];
    int rc;

    memset(bdf, 0, sizeof(bdf));
    snprintf(bdf, sizeof(bdf), "%s", String_val(vbdf));

    handle = calloc(1, sizeof(*handle));
    if (!handle)
        CAMLreturn(result_error("Out of memory"));

    handle->image_path = dup_string(Field(vconfig, 3));
    handle->serial = dup_string(Field(vconfig, 8));
    handle->model = dup_string(Field(vconfig, 9));
    if (!handle->image_path || !handle->serial || !handle->model) {
        handle_release(handle);
        free(handle);
        CAMLreturn(result_error("Out of memory"));
    }

    nvme_dev_config_defaults(&config);
    config.backend = Int_val(Field(vconfig, 0));
    config.admin_queue_depth = (uint16_t)Int_val(Field(vconfig, 1));
    config.enable_msix = Bool_val(Field(vconfig, 2));
    config.mock_image_path = handle->image_path;
    config.mock_capacity_bytes = (uint64_t)Int64_val(Field(vconfig, 4));
    config.mock_lba_bytes = (uint32_t)Int_val(Field(vconfig, 5));
    config.mock_namespaces = (uint32_t)Int_val(Field(vconfig, 6));
    config.mock_latency_us = (uint32_t)Int_val(Field(vconfig, 7));
    config.mock_serial = handle->serial;
    config.mock_model = handle->model;

    caml_release_runtime_system();
    rc = nvme_dev_open(bdf, &config, &dev);
    caml_acquire_runtime_system();

    if (rc != NVME_DMA_OK) {
        char message[160];
        snprintf(message, sizeof(message), "%s", nvme_strerror(rc));
        handle_release(handle);
        free(handle);
        CAMLreturn(result_error(message));
    }

    handle->dev = dev;
    handle->closed = 0;
    custom = caml_alloc_custom(&lantern_handle_ops, sizeof(lantern_handle_t *), 0, 1);
    Handle_val(custom) = handle;
    payload = custom;
    CAMLreturn(result_ok(payload));
}

CAMLprim value lantern_dev_close(value vdev)
{
    CAMLparam1(vdev);
    lantern_handle_t *handle = Handle_val(vdev);

    if (!handle || handle->closed)
        CAMLreturn(Val_int(NVME_DMA_OK));
    caml_release_runtime_system();
    nvme_dev_close(handle->dev);
    caml_acquire_runtime_system();
    handle->dev = NULL;
    handle->closed = 1;
    CAMLreturn(Val_int(NVME_DMA_OK));
}

static nvme_dev_t *device_of(value vdev)
{
    lantern_handle_t *handle = Handle_val(vdev);

    if (!handle || handle->closed)
        return NULL;
    return handle->dev;
}

CAMLprim value lantern_dev_reset(value vdev)
{
    CAMLparam1(vdev);
    nvme_dev_t *dev = device_of(vdev);
    int rc;

    if (!dev)
        CAMLreturn(Val_int(NVME_DMA_ERR_STATE));
    caml_release_runtime_system();
    rc = nvme_dev_reset(dev);
    caml_acquire_runtime_system();
    CAMLreturn(Val_int(rc));
}

CAMLprim value lantern_dev_error(value vdev)
{
    CAMLparam1(vdev);
    nvme_dev_t *dev = device_of(vdev);

    CAMLreturn(caml_copy_string(dev ? nvme_dev_error(dev) : "Device is closed"));
}

CAMLprim value lantern_dev_backend(value vdev)
{
    CAMLparam1(vdev);
    nvme_dev_t *dev = device_of(vdev);

    CAMLreturn(caml_copy_string(dev ? nvme_dev_backend_name(dev) : "closed"));
}

CAMLprim value lantern_dev_bdf(value vdev)
{
    CAMLparam1(vdev);
    nvme_dev_t *dev = device_of(vdev);

    CAMLreturn(caml_copy_string(dev ? nvme_dev_bdf(dev) : ""));
}

CAMLprim value lantern_strerror(value vcode)
{
    CAMLparam1(vcode);
    CAMLreturn(caml_copy_string(nvme_strerror(Int_val(vcode))));
}

CAMLprim value lantern_dev_state(value vdev)
{
    CAMLparam1(vdev);
    CAMLlocal1(record);
    nvme_dev_t *dev = device_of(vdev);
    nvme_dev_state_t state;

    memset(&state, 0, sizeof(state));
    if (dev)
        nvme_dev_state(dev, &state);

    record = caml_alloc(12, 0);
    Store_field(record, 0, caml_copy_int64((int64_t)state.cap));
    Store_field(record, 1, caml_copy_int32((int32_t)state.vs));
    Store_field(record, 2, caml_copy_int32((int32_t)state.cc));
    Store_field(record, 3, caml_copy_int32((int32_t)state.csts));
    Store_field(record, 4, caml_copy_int32((int32_t)state.aqa));
    Store_field(record, 5, caml_copy_int64((int64_t)state.asq));
    Store_field(record, 6, caml_copy_int64((int64_t)state.acq));
    Store_field(record, 7, Val_int(state.max_queue_entries));
    Store_field(record, 8, Val_int(state.doorbell_stride));
    Store_field(record, 9, Val_int(state.timeout_ms));
    Store_field(record, 10, Val_int(state.page_size));
    Store_field(record, 11, Val_int(state.max_transfer_bytes));
    CAMLreturn(record);
}

CAMLprim value lantern_queue_state(value vdev, value vqid)
{
    CAMLparam2(vdev, vqid);
    CAMLlocal1(record);
    nvme_dev_t *dev = device_of(vdev);
    nvme_queue_state_t state;

    memset(&state, 0, sizeof(state));
    if (dev)
        nvme_queue_state(dev, (uint16_t)Int_val(vqid), &state);

    record = caml_alloc(10, 0);
    Store_field(record, 0, Val_bool(state.active));
    Store_field(record, 1, Val_int(state.qid));
    Store_field(record, 2, Val_int(state.depth));
    Store_field(record, 3, Val_int(state.sq_tail));
    Store_field(record, 4, Val_int(state.cq_head));
    Store_field(record, 5, Val_int(state.cq_phase));
    Store_field(record, 6, caml_copy_int64((int64_t)state.sq_iova));
    Store_field(record, 7, caml_copy_int64((int64_t)state.cq_iova));
    Store_field(record, 8, caml_copy_int64((int64_t)state.submitted));
    Store_field(record, 9, caml_copy_int64((int64_t)state.completed));
    CAMLreturn(record);
}

CAMLprim value lantern_dma_alloc(value vdev, value vsize)
{
    CAMLparam2(vdev, vsize);
    nvme_dev_t *dev = device_of(vdev);
    nvme_dma_t *buffer = NULL;
    int rc;

    if (!dev)
        CAMLreturn(result_error("Device is closed"));
    rc = nvme_dma_alloc(dev, (size_t)Long_val(vsize), &buffer);
    if (rc != NVME_DMA_OK)
        CAMLreturn(result_error(nvme_dev_error(dev)[0] ? nvme_dev_error(dev) : nvme_strerror(rc)));
    CAMLreturn(result_ok(Val_int(nvme_dma_id(dev, buffer))));
}

CAMLprim value lantern_dma_free(value vdev, value vid)
{
    CAMLparam2(vdev, vid);
    nvme_dev_t *dev = device_of(vdev);
    nvme_dma_t *buffer = dev ? nvme_dma_handle(dev, Int_val(vid)) : NULL;

    if (buffer)
        nvme_dma_free(dev, buffer);
    CAMLreturn(Val_unit);
}

CAMLprim value lantern_dma_iova(value vdev, value vid)
{
    CAMLparam2(vdev, vid);
    nvme_dev_t *dev = device_of(vdev);
    nvme_dma_t *buffer = dev ? nvme_dma_handle(dev, Int_val(vid)) : NULL;

    CAMLreturn(caml_copy_int64((int64_t)nvme_dma_iova(buffer)));
}

CAMLprim value lantern_dma_size(value vdev, value vid)
{
    CAMLparam2(vdev, vid);
    nvme_dev_t *dev = device_of(vdev);
    nvme_dma_t *buffer = dev ? nvme_dma_handle(dev, Int_val(vid)) : NULL;

    CAMLreturn(Val_long((long)nvme_dma_size(buffer)));
}

CAMLprim value lantern_dma_write(value vdev, value vid, value voffset,
                                 value vsrc, value vsrc_offset, value vlen)
{
    CAMLparam5(vdev, vid, voffset, vsrc, vsrc_offset);
    CAMLxparam1(vlen);
    nvme_dev_t *dev = device_of(vdev);
    nvme_dma_t *buffer = dev ? nvme_dma_handle(dev, Int_val(vid)) : NULL;
    size_t len = (size_t)Long_val(vlen);
    size_t src_offset = (size_t)Long_val(vsrc_offset);
    int rc;

    if (!buffer)
        CAMLreturn(Val_int(NVME_DMA_ERR_STATE));
    if (src_offset + len > caml_string_length(vsrc))
        CAMLreturn(Val_int(NVME_DMA_ERR_RANGE));
    rc = nvme_dma_write(buffer, (size_t)Long_val(voffset), Bytes_val(vsrc) + src_offset, len);
    CAMLreturn(Val_int(rc));
}

CAMLprim value lantern_dma_write_bytecode(value *argv, int argn)
{
    (void)argn;
    return lantern_dma_write(argv[0], argv[1], argv[2], argv[3], argv[4], argv[5]);
}

CAMLprim value lantern_dma_read(value vdev, value vid, value voffset,
                                value vdst, value vdst_offset, value vlen)
{
    CAMLparam5(vdev, vid, voffset, vdst, vdst_offset);
    CAMLxparam1(vlen);
    nvme_dev_t *dev = device_of(vdev);
    nvme_dma_t *buffer = dev ? nvme_dma_handle(dev, Int_val(vid)) : NULL;
    size_t len = (size_t)Long_val(vlen);
    size_t dst_offset = (size_t)Long_val(vdst_offset);
    int rc;

    if (!buffer)
        CAMLreturn(Val_int(NVME_DMA_ERR_STATE));
    if (dst_offset + len > caml_string_length(vdst))
        CAMLreturn(Val_int(NVME_DMA_ERR_RANGE));
    rc = nvme_dma_read(buffer, (size_t)Long_val(voffset), Bytes_val(vdst) + dst_offset, len);
    CAMLreturn(Val_int(rc));
}

CAMLprim value lantern_dma_read_bytecode(value *argv, int argn)
{
    (void)argn;
    return lantern_dma_read(argv[0], argv[1], argv[2], argv[3], argv[4], argv[5]);
}

CAMLprim value lantern_dma_fill(value vdev, value vid, value voffset, value vbyte, value vlen)
{
    CAMLparam5(vdev, vid, voffset, vbyte, vlen);
    nvme_dev_t *dev = device_of(vdev);
    nvme_dma_t *buffer = dev ? nvme_dma_handle(dev, Int_val(vid)) : NULL;

    if (!buffer)
        CAMLreturn(Val_int(NVME_DMA_ERR_STATE));
    CAMLreturn(Val_int(nvme_dma_fill(buffer, (size_t)Long_val(voffset), Int_val(vbyte),
                                     (size_t)Long_val(vlen))));
}

CAMLprim value lantern_dma_pattern(value vdev, value vid, value voffset, value vlen, value vseed)
{
    CAMLparam5(vdev, vid, voffset, vlen, vseed);
    nvme_dev_t *dev = device_of(vdev);
    nvme_dma_t *buffer = dev ? nvme_dma_handle(dev, Int_val(vid)) : NULL;

    if (!buffer)
        CAMLreturn(Val_int(NVME_DMA_ERR_STATE));
    CAMLreturn(Val_int(nvme_dma_pattern(buffer, (size_t)Long_val(voffset),
                                        (size_t)Long_val(vlen), (uint64_t)Int64_val(vseed))));
}

CAMLprim value lantern_prp_build(value vdev, value vqid, value vcid, value vid,
                                 value voffset, value vlen, value vsqe)
{
    CAMLparam5(vdev, vqid, vcid, vid, voffset);
    CAMLxparam2(vlen, vsqe);
    nvme_dev_t *dev = device_of(vdev);
    nvme_dma_t *buffer = dev ? nvme_dma_handle(dev, Int_val(vid)) : NULL;
    nvme_sqe_t sqe;
    int rc;

    if (!buffer)
        CAMLreturn(Val_int(NVME_DMA_ERR_STATE));
    if (caml_string_length(vsqe) != sizeof(sqe))
        CAMLreturn(Val_int(NVME_DMA_ERR_INVALID));
    memcpy(&sqe, Bytes_val(vsqe), sizeof(sqe));
    rc = nvme_prp_build(dev, (uint16_t)Int_val(vqid), (uint16_t)Int_val(vcid), buffer,
                        (size_t)Long_val(voffset), (size_t)Long_val(vlen), &sqe);
    if (rc == NVME_DMA_OK)
        memcpy(Bytes_val(vsqe), &sqe, sizeof(sqe));
    CAMLreturn(Val_int(rc));
}

CAMLprim value lantern_prp_build_bytecode(value *argv, int argn)
{
    (void)argn;
    return lantern_prp_build(argv[0], argv[1], argv[2], argv[3], argv[4], argv[5], argv[6]);
}

static int submit_common(value vdev, uint16_t qid, value vsqe)
{
    nvme_dev_t *dev = device_of(vdev);
    nvme_sqe_t sqe;

    if (!dev)
        return NVME_DMA_ERR_STATE;
    if (caml_string_length(vsqe) != sizeof(sqe))
        return NVME_DMA_ERR_INVALID;
    memcpy(&sqe, Bytes_val(vsqe), sizeof(sqe));
    return qid == 0 ? nvme_admin_submit(dev, &sqe) : nvme_io_submit(dev, qid, &sqe);
}

CAMLprim value lantern_admin_submit(value vdev, value vsqe)
{
    CAMLparam2(vdev, vsqe);
    CAMLreturn(Val_int(submit_common(vdev, 0, vsqe)));
}

CAMLprim value lantern_io_submit(value vdev, value vqid, value vsqe)
{
    CAMLparam3(vdev, vqid, vsqe);
    CAMLreturn(Val_int(submit_common(vdev, (uint16_t)Int_val(vqid), vsqe)));
}

static value poll_result(int rc, const nvme_cqe_t *cqe)
{
    CAMLparam0();
    CAMLlocal2(block, record);

    if (rc == 0)
        CAMLreturn(Val_int(0));
    if (rc < 0) {
        block = caml_alloc(1, 1);
        Store_field(block, 0, Val_int(rc));
        CAMLreturn(block);
    }
    record = caml_alloc(5, 0);
    Store_field(record, 0, caml_copy_int32((int32_t)cqe->dw0));
    Store_field(record, 1, Val_int(cqe->sq_head));
    Store_field(record, 2, Val_int(cqe->sq_id));
    Store_field(record, 3, Val_int(cqe->cid));
    Store_field(record, 4, Val_int(cqe->status));
    block = caml_alloc(1, 0);
    Store_field(block, 0, record);
    CAMLreturn(block);
}

CAMLprim value lantern_admin_poll(value vdev, value vtimeout)
{
    CAMLparam2(vdev, vtimeout);
    nvme_dev_t *dev = device_of(vdev);
    nvme_cqe_t cqe;
    int timeout = Int_val(vtimeout);
    int rc;

    memset(&cqe, 0, sizeof(cqe));
    if (!dev)
        CAMLreturn(poll_result(NVME_DMA_ERR_STATE, &cqe));
    if (timeout == 0) {
        rc = nvme_admin_poll(dev, &cqe, 0);
    } else {
        caml_release_runtime_system();
        rc = nvme_admin_poll(dev, &cqe, timeout);
        caml_acquire_runtime_system();
    }
    CAMLreturn(poll_result(rc, &cqe));
}

CAMLprim value lantern_io_poll(value vdev, value vqid, value vtimeout)
{
    CAMLparam3(vdev, vqid, vtimeout);
    nvme_dev_t *dev = device_of(vdev);
    nvme_cqe_t cqe;
    uint16_t qid = (uint16_t)Int_val(vqid);
    int timeout = Int_val(vtimeout);
    int rc;

    memset(&cqe, 0, sizeof(cqe));
    if (!dev)
        CAMLreturn(poll_result(NVME_DMA_ERR_STATE, &cqe));
    if (timeout == 0) {
        rc = nvme_io_poll(dev, qid, &cqe, 0);
    } else {
        caml_release_runtime_system();
        rc = nvme_io_poll(dev, qid, &cqe, timeout);
        caml_acquire_runtime_system();
    }
    CAMLreturn(poll_result(rc, &cqe));
}

CAMLprim value lantern_io_queue_alloc(value vdev, value vqid, value vdepth)
{
    CAMLparam3(vdev, vqid, vdepth);
    nvme_dev_t *dev = device_of(vdev);

    if (!dev)
        CAMLreturn(Val_int(NVME_DMA_ERR_STATE));
    CAMLreturn(Val_int(nvme_io_queue_alloc(dev, (uint16_t)Int_val(vqid), (uint16_t)Int_val(vdepth))));
}

CAMLprim value lantern_io_queue_addresses(value vdev, value vqid)
{
    CAMLparam2(vdev, vqid);
    CAMLlocal3(pair, sq, cq);
    nvme_dev_t *dev = device_of(vdev);
    uint64_t sq_iova = 0;
    uint64_t cq_iova = 0;
    int rc;

    if (!dev)
        CAMLreturn(result_error("Device is closed"));
    rc = nvme_io_queue_addresses(dev, (uint16_t)Int_val(vqid), &sq_iova, &cq_iova);
    if (rc != NVME_DMA_OK)
        CAMLreturn(result_error(nvme_strerror(rc)));
    sq = caml_copy_int64((int64_t)sq_iova);
    cq = caml_copy_int64((int64_t)cq_iova);
    pair = caml_alloc(2, 0);
    Store_field(pair, 0, sq);
    Store_field(pair, 1, cq);
    CAMLreturn(result_ok(pair));
}

CAMLprim value lantern_io_queue_activate(value vdev, value vqid)
{
    CAMLparam2(vdev, vqid);
    nvme_dev_t *dev = device_of(vdev);

    if (!dev)
        CAMLreturn(Val_int(NVME_DMA_ERR_STATE));
    CAMLreturn(Val_int(nvme_io_queue_activate(dev, (uint16_t)Int_val(vqid))));
}

CAMLprim value lantern_io_queue_free(value vdev, value vqid)
{
    CAMLparam2(vdev, vqid);
    nvme_dev_t *dev = device_of(vdev);

    if (!dev)
        CAMLreturn(Val_int(NVME_DMA_ERR_STATE));
    CAMLreturn(Val_int(nvme_io_queue_free(dev, (uint16_t)Int_val(vqid))));
}

CAMLprim value lantern_irq_enable(value vdev, value vcount)
{
    CAMLparam2(vdev, vcount);
    nvme_dev_t *dev = device_of(vdev);

    if (!dev)
        CAMLreturn(Val_int(NVME_DMA_ERR_STATE));
    CAMLreturn(Val_int(nvme_irq_enable(dev, (unsigned)Int_val(vcount))));
}

CAMLprim value lantern_irq_disable(value vdev)
{
    CAMLparam1(vdev);
    nvme_dev_t *dev = device_of(vdev);

    if (!dev)
        CAMLreturn(Val_int(NVME_DMA_ERR_STATE));
    CAMLreturn(Val_int(nvme_irq_disable(dev)));
}

CAMLprim value lantern_irq_fd(value vdev, value vvector)
{
    CAMLparam2(vdev, vvector);
    nvme_dev_t *dev = device_of(vdev);

    CAMLreturn(Val_int(dev ? nvme_irq_fd(dev, (unsigned)Int_val(vvector)) : -1));
}

CAMLprim value lantern_irq_count(value vdev)
{
    CAMLparam1(vdev);
    nvme_dev_t *dev = device_of(vdev);

    CAMLreturn(Val_int(dev ? (int)nvme_irq_count(dev) : 0));
}

CAMLprim value lantern_irq_wait(value vdev, value vvector, value vtimeout)
{
    CAMLparam3(vdev, vvector, vtimeout);
    nvme_dev_t *dev = device_of(vdev);
    int rc;

    if (!dev)
        CAMLreturn(Val_int(NVME_DMA_ERR_STATE));
    caml_release_runtime_system();
    rc = nvme_irq_wait(dev, (unsigned)Int_val(vvector), Int_val(vtimeout));
    caml_acquire_runtime_system();
    CAMLreturn(Val_int(rc));
}
