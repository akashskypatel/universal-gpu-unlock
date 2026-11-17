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
/* Retain backing-object identities across completed and failed GPU steps.
 * This is a reuse guard, not proof that a driver Stop is safe. */
typedef struct {
  const void *platform;
  const void *release_plan;
  const void *firmware_assets;
} GU_GPU_EXEC_OWNERS;
typedef struct {
  GU_GPU_RECORD identity;
  uint32_t expected_config[64];
  GU_GPU_EXEC_PHASE phase;
  GU_GPU_EXEC_OWNERS owners;
} GU_GPU_EXEC_TARGET;
typedef struct {
  size_t count, selected_count, next_step, active_step;
  int halted, owned_mode;
  size_t capacity;
  size_t *order;
  GU_GPU_EXEC_TARGET *targets;
} GU_GPU_EXEC_SEQUENCE;
#define GU_GPU_EXEC_NO_STEP SIZE_MAX
enum {
  GU_GPU_EXEC_OK=0, GU_GPU_EXEC_INVALID=1,
  GU_GPU_EXEC_DRIFT=2, GU_GPU_EXEC_BLOCKED=3
};
/* The caller supplies two arrays of capacity elements. Their storage must
 * remain valid and independent of the input arrays for the sequence lifetime.
 * Do not copy a sequence to create another independent owner. */
int GuGpuExecInit(GU_GPU_EXEC_SEQUENCE *state,
                  const GU_GPU_EXEC_OBSERVATION *all, size_t count,
                  const size_t *order, size_t selected_count,
                  GU_GPU_EXEC_TARGET *target_storage, size_t *order_storage,
                  size_t capacity);
/* Resolve the binding to the pending next step, the active step, or the
 * most recently completed step (while its context remains bound).
 * Match both immutable boot-local references. No hardware authorization. */
int GuGpuExecResolveBound(const GU_GPU_EXEC_SEQUENCE *state,
                         const void *handle,const void *pci,
                         size_t *step,size_t *target);
int GuGpuExecCheckNext(const GU_GPU_EXEC_SEQUENCE *state,
                       const GU_GPU_EXEC_OBSERVATION *all, size_t count);
/* CPU-only final observation check; does not establish hardware safety. */
int GuGpuExecCheckComplete(const GU_GPU_EXEC_SEQUENCE *state,
                           const GU_GPU_EXEC_OBSERVATION *all, size_t count);
int GuGpuExecBegin(GU_GPU_EXEC_SEQUENCE *state, size_t step,
                   const GU_GPU_EXEC_OBSERVATION *all, size_t count);
/* Live active entrypoints must pass distinct genuine backing-object tokens. */
int GuGpuExecBeginOwned(GU_GPU_EXEC_SEQUENCE *state, size_t step,
                        const GU_GPU_EXEC_OBSERVATION *all, size_t count,
                        const void *platform, const void *release_plan,
                        const void *firmware_assets);
/* A completion claim requires independent real cleanup/handover evidence. */
int GuGpuExecFinish(GU_GPU_EXEC_SEQUENCE *state, size_t step,
                    const GU_GPU_EXEC_OBSERVATION *all, size_t count,
                    int cleanup_and_handover_verified);
void GuGpuExecAbort(GU_GPU_EXEC_SEQUENCE *state);
