/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef GPU_UNLOCK_UNLOCK_610_H
#define GPU_UNLOCK_UNLOCK_610_H

struct firmware;
struct pci_dev;

/* Borrowed immutable firmware reference for one open GPU scope.
 * The data remains owned by that GPU's platform, not by this record. */
struct gpu_unlock_610_shared {
	const struct firmware *gsp;
	const struct pci_dev *owner;
};

/* Load the reference into an empty record for this device. */
int gpu_unlock_610_load_shared(struct pci_dev *pdev, struct gpu_unlock_610_shared *shared);

/* Consume a matching borrow; do not free platform-owned data. */
int gpu_unlock_610_release_shared(struct pci_dev *pdev, struct gpu_unlock_610_shared *shared);

/* Execute one checked transaction with its own borrowed firmware reference.
 * This interface does not authorize concurrent transactions. */
int gpu_unlock_610_unlock(struct pci_dev *pdev, const struct gpu_unlock_610_shared *shared);

#endif
