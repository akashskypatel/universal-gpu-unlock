/* SPDX-License-Identifier: GPL-2.0-only */
#pragma once
#include "gpu_inventory.h"

/* A CPU-owned record of ordered GPU steps. This never authorizes Stop, DMA,
 * PCI writes, FLR or secure firmware execution. */
typedef struct {
  GU_GPU_RECORD identity;
  uint32_t config[64];
} GU_GPU_EXEC_OBSERVATION;
typedef enum {
  GU_GPU_EXEC_PENDING=0, GU_GPU_EXEC_ACTIVE,
  GU_GPU_EXEC_COMPLETE, GU_GPU_EXEC_FAILED
} GU_GPU_EXEC_PHASE;
typedef struct {
  GU_GPU_RECORD identity;
  uint32_t expected_config[64];
  GU_GPU_EXEC_PHASE phase;
} GU_GPU_EXEC_TARGET;
typedef struct {
  size_t count, selected_count, next_step, active_step;
  int halted;
  size_t order[GU_INVENTORY_MAX_NVIDIA];
  GU_GPU_EXEC_TARGET targets[GU_INVENTORY_MAX_NVIDIA];
} GU_GPU_EXEC_SEQUENCE;
enum {
  GU_GPU_EXEC_OK=0, GU_GPU_EXEC_INVALID=1,
  GU_GPU_EXEC_DRIFT=2, GU_GPU_EXEC_BLOCKED=3
};
int GuGpuExecInit(GU_GPU_EXEC_SEQUENCE *state,
                  const GU_GPU_EXEC_OBSERVATION *all, size_t count,
                  const size_t *order, size_t selected_count);
int GuGpuExecCheckNext(const GU_GPU_EXEC_SEQUENCE *state,
                       const GU_GPU_EXEC_OBSERVATION *all, size_t count);
int GuGpuExecBegin(GU_GPU_EXEC_SEQUENCE *state, size_t step,
                   const GU_GPU_EXEC_OBSERVATION *all, size_t count);
/* A completion claim requires independent real cleanup/handover evidence. */
int GuGpuExecFinish(GU_GPU_EXEC_SEQUENCE *state, size_t step,
                    const GU_GPU_EXEC_OBSERVATION *all, size_t count,
                    int cleanup_and_handover_verified);
void GuGpuExecAbort(GU_GPU_EXEC_SEQUENCE *state);
