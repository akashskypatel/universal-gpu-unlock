#pragma once
#include "../port.h"
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
typedef UINT8 u8;
typedef UINT16 u16;
typedef UINT32 u32;
typedef UINT64 u64;
typedef INT32 s32;
typedef UINT64 dma_addr_t;
typedef UINT64 resource_size_t;
typedef UINT64 ktime_t;
#define __iomem
#define GFP_KERNEL 0
#ifndef ARRAY_SIZE
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
#endif
#define min_t(t,a,b) ((t)(a)<(t)(b)?(t)(a):(t)(b))
#define lower_32_bits(v) ((u32)(v))
#define upper_32_bits(v) ((u32)((u64)(v)>>32))
/* A 64-bit DMA mask must not shift a 64-bit word by 64 bits. */
#define DMA_BIT_MASK(n) (((n) == 64) ? ~0ull : ((1ull<<(n))-1))
#define MODULE_FIRMWARE(x)
#define THIS_MODULE NULL
#define dev_err(dev, ...) GuLog(__VA_ARGS__)
#define dev_info(dev, ...) GuLog(__VA_ARGS__)
#define pr_err(...) GuLog(__VA_ARGS__)
#define pr_info(...) GuLog(__VA_ARGS__)
#define PCI_D0 0
#define PCI_DEV_FLAGS_NO_FLR_RESET (1u<<10)
#define PCI_VENDOR_ID 0
#define PCI_COMMAND 4
#define PCI_COMMAND_MEMORY 2
#define PCI_COMMAND_MASTER 4
#define PCI_PM_CTRL 4
#define PCI_PM_CTRL_STATE_MASK 3
#define PCI_EXP_DEVCAP 4
#define PCI_EXP_DEVCAP_FLR (1u<<28)
#define PCI_EXP_DEVSTA 10
#define PCI_EXP_DEVSTA_TRPND 32
#define PCI_BASE_ADDRESS_0 16
#define IORESOURCE_MEM 0x200
/* Stable Linux errno values used by the retained transaction. */
#define EIO 5
#define ENOMEM 12
#define EBUSY 16
#define ENODEV 19
#define EINVAL 22
#define ERANGE 34
#define EAGAIN 11
#define EPROTO 71
#define EBADMSG 74
#define EOPNOTSUPP 95
#define ETIMEDOUT 110
#define EOVERFLOW 75
#define ENOENT 2
#define EALREADY 114
struct list_head {unsigned int count;};
static inline bool list_empty(const struct list_head *h) {return !h->count;}
#define spin_lock_irqsave(lock, flags) do {(void)(lock);(flags)=0;} while(0)
#define spin_unlock_irqrestore(lock, flags) do {(void)(lock);(void)(flags);} while(0)
struct device {u64 *dma_mask;u64 coherent_dma_mask;struct list_head devres_head;int devres_lock;bool scope;};

#define PCI_UNKNOWN -1
struct pci_dev {
  u16 vendor,device;struct device dev;int current_state,pm_cap,refs;
  unsigned int dev_flags;
  GU_PLATFORM *owner;
  EFI_BOOT_SERVICES *owner_bs;
  EFI_PCI_IO_PROTOCOL *owner_pci;
  EFI_HANDLE owner_handle;
  u64 mask_storage;
};
/* CPU-only transaction identity. Reserve before release. Open only after
 * existing ownership and cold-entry checks. Neither grants hardware admission. */
EFI_STATUS GuCompatReserveDevice(VOID);
EFI_STATUS GuCompatOpenDevice(struct pci_dev **out);
EFI_STATUS GuCompatCloseDevice(struct pci_dev *dev);
/* Resolve only the validated open owner. Does not select or rebind a GPU. */
GU_PLATFORM *GuCompatOwner(const struct pci_dev *dev);
BOOLEAN GuCompatPciMatches(const struct pci_dev *dev);
/* The table always belongs to the current platform, even before reservation. */
GU_ASSET *GuAssetEntries(VOID);
/* Reserve the current platform's empty table once. */
EFI_STATUS GuAssetReserveEntries(EFI_BOOT_SERVICES *bs);
#define GuAssets (GuAssetEntries())
void *memcpy(void *,const void *,size_t);
void *memset(void *,int,size_t);
int memcmp(const void *,const void *,size_t);
void *memchr(const void *,int,size_t);
int strcmp(const char *,const char *);
size_t strlen(const char *);
void *devres_open_group(struct device *,void *,int);
int devres_release_group(struct device *,void *);
ktime_t GuCompatTime(const struct pci_dev *);
BOOLEAN GuCompatDelay(const struct pci_dev *,UINTN);
static inline ktime_t ktime_add_us(ktime_t t,unsigned long u) {return t+u;}
static inline int ktime_compare(ktime_t a,ktime_t b) {return a>b?1:a<b?-1:0;}
/* Device-attributed transaction I/O. The device must own the open scope. */
u32 GuCompatRead32(const struct pci_dev *,const void *);
void GuCompatWrite32(const struct pci_dev *,u32,void *);
BOOLEAN GuCompatCopyFromIo(const struct pci_dev *,void *,const void *,size_t);
void *dma_alloc_coherent(struct device *,size_t,dma_addr_t *,int);
void dma_free_coherent(struct device *,size_t,void *,dma_addr_t);
int dma_set_mask(struct device *,u64);
int dma_set_coherent_mask(struct device *,u64);
void GuCompatDmaBarrier(const struct pci_dev *);
void *GuCompatAlloc(const struct pci_dev *,size_t,size_t);
BOOLEAN GuCompatFree(const struct pci_dev *,void *);
void GuCompatRetain(const struct pci_dev *);
int request_firmware(const struct firmware **,const char *,struct device *);
void release_firmware(const struct firmware *);
int pci_read_config_word(struct pci_dev *,int,u16 *);
int pci_read_config_dword(struct pci_dev *,int,u32 *);
int pci_write_config_word(struct pci_dev *,int,u16);
int pcie_capability_read_word(struct pci_dev *,int,u16 *);
int pcie_capability_read_dword(struct pci_dev *,int,u32 *);
int pci_is_pcie(struct pci_dev *);
int pci_select_bars(struct pci_dev *,int);
int pci_request_region(struct pci_dev *,int,const char *);
void pci_release_region(struct pci_dev *,int);
int aperture_remove_conflicting_pci_devices(struct pci_dev *,const char *);
int pci_enable_device_mem(struct pci_dev *);
void pci_disable_device(struct pci_dev *);
resource_size_t pci_resource_len(struct pci_dev *,int);
void *pci_iomap(struct pci_dev *,int,unsigned long);
void pci_iounmap(struct pci_dev *,void *);
int pci_save_state(struct pci_dev *);
void pci_restore_state(struct pci_dev *);
void pci_set_master(struct pci_dev *);
void pci_clear_master(struct pci_dev *);
int pcie_flr(struct pci_dev *);
struct pci_dev *pci_dev_get(struct pci_dev *);
void pci_dev_put(struct pci_dev *);
