#ifndef LANTERN_SPEC_H
#define LANTERN_SPEC_H

#include <stdint.h>

#define NVME_REG_CAP    0x0000
#define NVME_REG_VS     0x0008
#define NVME_REG_INTMS  0x000c
#define NVME_REG_INTMC  0x0010
#define NVME_REG_CC     0x0014
#define NVME_REG_CSTS   0x001c
#define NVME_REG_NSSR   0x0020
#define NVME_REG_AQA    0x0024
#define NVME_REG_ASQ    0x0028
#define NVME_REG_ACQ    0x0030
#define NVME_REG_DOORBELL_BASE 0x1000

#define NVME_CAP_MQES(c)   ((uint32_t)(((c) & 0xffffu) + 1u))
#define NVME_CAP_CQR(c)    ((uint32_t)(((c) >> 16) & 0x1u))
#define NVME_CAP_TO(c)     ((uint32_t)(((c) >> 24) & 0xffu))
#define NVME_CAP_DSTRD(c)  ((uint32_t)(((c) >> 32) & 0xfu))
#define NVME_CAP_CSS(c)    ((uint32_t)(((c) >> 37) & 0xffu))
#define NVME_CAP_MPSMIN(c) ((uint32_t)(((c) >> 48) & 0xfu))
#define NVME_CAP_MPSMAX(c) ((uint32_t)(((c) >> 52) & 0xfu))

#define NVME_CC_EN        (1u << 0)
#define NVME_CC_CSS_SHIFT 4
#define NVME_CC_MPS_SHIFT 7
#define NVME_CC_AMS_SHIFT 11
#define NVME_CC_SHN_SHIFT 14
#define NVME_CC_IOSQES_SHIFT 16
#define NVME_CC_IOCQES_SHIFT 20

#define NVME_CSTS_RDY   (1u << 0)
#define NVME_CSTS_CFS   (1u << 1)
#define NVME_CSTS_SHST_SHIFT 2

#define NVME_ADMIN_DELETE_SQ   0x00
#define NVME_ADMIN_CREATE_SQ   0x01
#define NVME_ADMIN_GET_LOG     0x02
#define NVME_ADMIN_DELETE_CQ   0x04
#define NVME_ADMIN_CREATE_CQ   0x05
#define NVME_ADMIN_IDENTIFY    0x06
#define NVME_ADMIN_ABORT       0x08
#define NVME_ADMIN_SET_FEATURES 0x09
#define NVME_ADMIN_GET_FEATURES 0x0a
#define NVME_ADMIN_NS_MANAGEMENT 0x0d
#define NVME_ADMIN_NS_ATTACHMENT 0x15
#define NVME_ADMIN_FORMAT_NVM  0x80

#define NVME_IO_FLUSH   0x00
#define NVME_IO_WRITE   0x01
#define NVME_IO_READ    0x02
#define NVME_IO_WRITE_ZEROES 0x08

#define NVME_IDENTIFY_CNS_NAMESPACE  0x00
#define NVME_IDENTIFY_CNS_CONTROLLER 0x01
#define NVME_IDENTIFY_CNS_NS_LIST    0x02

#define NVME_FEATURE_NUM_QUEUES 0x07

#define NVME_SCT_GENERIC        0x0
#define NVME_SCT_COMMAND_SPECIFIC 0x1
#define NVME_SCT_MEDIA          0x2

#define NVME_SC_SUCCESS             0x00
#define NVME_SC_INVALID_OPCODE      0x01
#define NVME_SC_INVALID_FIELD       0x02
#define NVME_SC_DATA_TRANSFER_ERROR 0x04
#define NVME_SC_INTERNAL_ERROR      0x06
#define NVME_SC_ABORT_REQUESTED     0x07
#define NVME_SC_INVALID_NAMESPACE   0x0b
#define NVME_SC_LBA_OUT_OF_RANGE    0x80
#define NVME_SC_CAPACITY_EXCEEDED   0x81

#define NVME_SC_CS_COMPLETION_QUEUE_INVALID 0x00
#define NVME_SC_CS_INVALID_QUEUE_IDENTIFIER 0x01
#define NVME_SC_CS_INVALID_QUEUE_SIZE       0x02
#define NVME_SC_CS_INVALID_QUEUE_DELETION   0x0c
#define NVME_SC_CS_NS_IDENTIFIER_UNAVAILABLE 0x16
#define NVME_SC_CS_NS_ALREADY_ATTACHED      0x18
#define NVME_SC_CS_NS_NOT_ATTACHED          0x1a

#define NVME_CQE_STATUS_PHASE(s)  ((s) & 0x1u)
#define NVME_CQE_STATUS_SC(s)     (((s) >> 1) & 0xffu)
#define NVME_CQE_STATUS_SCT(s)    (((s) >> 9) & 0x7u)
#define NVME_CQE_STATUS_DNR(s)    (((s) >> 15) & 0x1u)

#define NVME_PAGE_SHIFT 12
#define NVME_PAGE_SIZE  (1u << NVME_PAGE_SHIFT)
#define NVME_PAGE_MASK  ((uint64_t)(NVME_PAGE_SIZE - 1))

#define NVME_SQE_BYTES 64
#define NVME_CQE_BYTES 16

struct nvme_wire_sqe {
    uint8_t  opcode;
    uint8_t  flags;
    uint16_t cid;
    uint32_t nsid;
    uint32_t cdw2;
    uint32_t cdw3;
    uint64_t metadata;
    uint64_t prp1;
    uint64_t prp2;
    uint32_t cdw10;
    uint32_t cdw11;
    uint32_t cdw12;
    uint32_t cdw13;
    uint32_t cdw14;
    uint32_t cdw15;
};

struct nvme_wire_cqe {
    uint32_t dw0;
    uint32_t dw1;
    uint16_t sq_head;
    uint16_t sq_id;
    uint16_t cid;
    uint16_t status;
};

struct nvme_lbaf {
    uint16_t ms;
    uint8_t  lbads;
    uint8_t  rp;
};

struct nvme_id_ctrl {
    uint16_t vid;
    uint16_t ssvid;
    char     sn[20];
    char     mn[40];
    char     fr[8];
    uint8_t  rab;
    uint8_t  ieee[3];
    uint8_t  cmic;
    uint8_t  mdts;
    uint16_t cntlid;
    uint32_t ver;
    uint8_t  reserved84[172];
    uint16_t oacs;
    uint8_t  acl;
    uint8_t  aerl;
    uint8_t  frmw;
    uint8_t  lpa;
    uint8_t  elpe;
    uint8_t  npss;
    uint8_t  avscc;
    uint8_t  apsta;
    uint16_t wctemp;
    uint16_t cctemp;
    uint8_t  reserved270[242];
    uint8_t  sqes;
    uint8_t  cqes;
    uint16_t maxcmd;
    uint32_t nn;
    uint16_t oncs;
    uint16_t fuses;
    uint8_t  fna;
    uint8_t  vwc;
    uint16_t awun;
    uint16_t awupf;
    uint8_t  reserved530[1518];
    uint8_t  psd[1024];
    uint8_t  vs[1024];
};

struct nvme_id_ns {
    uint64_t nsze;
    uint64_t ncap;
    uint64_t nuse;
    uint8_t  nsfeat;
    uint8_t  nlbaf;
    uint8_t  flbas;
    uint8_t  mc;
    uint8_t  dpc;
    uint8_t  dps;
    uint8_t  nmic;
    uint8_t  rescap;
    uint8_t  fpi;
    uint8_t  dlfeat;
    uint16_t nawun;
    uint16_t nawupf;
    uint16_t nacwu;
    uint8_t  reserved32[8];
    uint64_t nvmcap[2];
    uint8_t  reserved64[40];
    uint8_t  nguid[16];
    uint8_t  eui64[8];
    struct nvme_lbaf lbaf[16];
    uint8_t  reserved192[192];
    uint8_t  vs[3712];
};

#endif
