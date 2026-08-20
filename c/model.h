#ifndef LANTERN_MODEL_H
#define LANTERN_MODEL_H

#include <stddef.h>
#include <stdint.h>

#include "vfio.h"

typedef struct nvme_model nvme_model_t;

nvme_model_t *nvme_model_create(const lt_mock_config_t *cfg, char *errbuf, size_t errlen);
void          nvme_model_destroy(nvme_model_t *model);

void   *nvme_model_bar(nvme_model_t *model);
size_t  nvme_model_bar_size(nvme_model_t *model);
uint8_t *nvme_model_config_space(nvme_model_t *model);
size_t  nvme_model_config_size(nvme_model_t *model);

int  nvme_model_dma_map(nvme_model_t *model, uint64_t iova, uint64_t vaddr, uint64_t size);
int  nvme_model_dma_unmap(nvme_model_t *model, uint64_t iova, uint64_t size);
int  nvme_model_set_msix(nvme_model_t *model, uint32_t start, uint32_t count, const int32_t *fds);
void nvme_model_device_reset(nvme_model_t *model);

uint32_t nvme_model_msix_count(nvme_model_t *model);
uint64_t nvme_model_commands_executed(nvme_model_t *model);

#endif
