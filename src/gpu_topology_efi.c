/* SPDX-License-Identifier: GPL-2.0-only */
/* Standalone diagnostic UEFI application: NO unlock, disconnect, MMIO, DMA,
 * PCI config writes, FLR, controller manipulation, or NVRAM writes. */
#if !defined(GU_GPU_DIAGNOSTIC_ONLY) || defined(GU_AD104_TRANSACTION) || \
    defined(GU_ENTRY_LATCHED_SS1_FULL) || defined(GU_FULL_UNLOCK)
#error This file may only be built as a standalone read-only diagnostic
#endif
#include <Uefi.h>
#include <Protocol/PciIo.h>
#include <Protocol/DevicePath.h>
#include <Protocol/GraphicsOutput.h>
#include <Protocol/SimpleTextOut.h>
#include <Protocol/LoadedImage.h>
#include <Protocol/DriverBinding.h>
#ifdef GU_TOPO_SAVE_LOG
#include <Protocol/SimpleFileSystem.h>
#endif
#include "gpu_topology.h"
#include "sha256.h"
#include "gpu_loaded_pe_text.h"
#ifdef GU_TOPO_SELECTION_ENABLED
#ifdef GU_TOPO_WINDOWS_HANDOFF
#error Selection-only diagnostic cannot automatically hand off to Windows
#endif
#include "gpu-selection.h" /* Generated from operator-supplied PCI locations. */
#if GU_GPU_SELECTION_COUNT < 1 || GU_GPU_SELECTION_COUNT > GU_INVENTORY_MAX_NVIDIA
#error Invalid number of selected PCI locations
#endif
static const GU_GPU_LOCATION SelectedLocations[GU_GPU_SELECTION_COUNT] =
    GU_GPU_SELECTION_LOCATIONS;
#endif
#include "gpu_pci_observation.h"
#ifdef GU_MSI_B850_ROM_VARIABLES
#include "msi_b850_rom_diag.h"
#if defined(GU_TOPO_SELECTION_ENABLED) || defined(GU_TOPO_WINDOWS_HANDOFF)
#error MSI B850 variable diagnostic cannot select GPUs or hand off Windows
#endif
#endif
#ifdef GU_TOPO_WINDOWS_HANDOFF
#include "gpu_windows_handoff.h"
#endif
#include "format.h"
#include <stdarg.h>
#include <string.h>

static EFI_GUID PciGuid=EFI_PCI_IO_PROTOCOL_GUID;
static EFI_GUID PathGuid=EFI_DEVICE_PATH_PROTOCOL_GUID;
static EFI_GUID GopGuid=EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
static EFI_GUID TextOutGuid=EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL_GUID;
static EFI_GUID DriverBindingGuid=EFI_DRIVER_BINDING_PROTOCOL_GUID;
static EFI_GUID LoadedGuid=EFI_LOADED_IMAGE_PROTOCOL_GUID;
#ifdef GU_MSI_B850_ROM_VARIABLES
static EFI_GUID LoadedImagePathGuid=EFI_LOADED_IMAGE_DEVICE_PATH_PROTOCOL_GUID;
#endif
static EFI_GUID GlobalGuid={0x8be4df61,0x93ca,0x11d2,{0xaa,0x0d,0,0xe0,0x98,0x03,0x2b,0x8c}};
static GU_TOPO_CAPTURE Snapshot;
/* The underlying GOP handles are boot-local: never persist or use for Stop. */
static EFI_HANDLE GopHandles[GU_TOPO_MAX_GOPS];
static GU_TOPO_REPORT Report;
#ifdef GU_TOPO_SELECTION_ENABLED
static int SelectionOk;
#endif
#ifdef GU_TOPO_SELECTION_ENABLED
/* Uses only already captured PCI identities; no fresh MMIO or EFI mutation. */
static int SelectionRead(void *opaque, size_t index, GU_GPU_RECORD *out) {
  const GU_TOPO_CAPTURE *capture=(const GU_TOPO_CAPTURE *)opaque;
  if(!capture || !out || index>=capture->pci_count)return -1;
  *out=capture->pci[index].identity;
  return 0;
}
#endif
/* Independent, read-only *evidence*, never used as a topology pass. */
static GU_PCI_OBS_CAPTURE Supplemental;
static EFI_SYSTEM_TABLE *System;
#ifdef GU_TOPO_SAVE_LOG
static char LogBytes[131072];
static size_t LogUsed;
static int LogOverflow;
static EFI_GUID FsGuid=EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
#endif

static void Say(const char *format, ...) {
  char line[320];CHAR16 wide[321];size_t i,n;
  va_list ap;va_start(ap,format);n=GuFormatV(line,sizeof(line),format,ap);va_end(ap);
  if(n==SIZE_MAX || n>=sizeof(line))return;
#ifdef GU_TOPO_SAVE_LOG
  if(n>sizeof(LogBytes)-LogUsed)LogOverflow=1;
  else {memcpy(LogBytes+LogUsed,line,n);LogUsed+=n;}
#endif
  for(i=0;i<n;i++)wide[i]=(CHAR16)(unsigned char)line[i];wide[n]=0;
  if(System && System->ConOut && System->ConOut->OutputString)
    (VOID)System->ConOut->OutputString(System->ConOut,wide);
}


#ifdef GU_MSI_B850_ROM_VARIABLES
/* MSI 1A70 ROM_CMN and CSM_CMN: GetVariable only, before ExitBootServices.
 * A captured mode or UEFI status does not authorize original NVIDIA Stop. */
static EFI_GUID MsiB850RomGuid =
  {0x97cdc6be,0x7792,0x4782,{0xba,0x6d,0xe6,0xe3,0xbb,0x56,0xac,0xd8}};
#define GU_MSI_B850_VAR_CAP 1024U

static void ReportMsiB850Variable(EFI_SYSTEM_TABLE *table,
                                  CHAR16 *name, const char *label) {
  UINT8 data[GU_MSI_B850_VAR_CAP];
  UINTN bytes=sizeof(data),off;
  UINT32 attrs=0;
  EFI_STATUS s;
  static const char digits[]="0123456789ABCDEF";
  if(!table->RuntimeServices || !table->RuntimeServices->GetVariable) {
    Say("MSI_B850_VARIABLE NAME=%s STATUS=NO_GET_VARIABLE READ_ONLY=1\r\n",label);
    return;
  }
  memset(data,0,sizeof(data));
  s=table->RuntimeServices->GetVariable(name,&MsiB850RomGuid,
                                         &attrs,&bytes,data);
  Say("MSI_B850_VARIABLE NAME=%s EFI_STATUS=%#llx BYTES=%zu ATTRS=%#x CAP=%u RAW_ONLY=1 READ_ONLY=1\r\n",
      label,(unsigned long long)s,(size_t)bytes,
      (unsigned)attrs,(unsigned)sizeof(data));
  if(s!=EFI_SUCCESS) {
    Say("MSI_B850_VARIABLE_RESULT NAME=%s STATE=%s NO_DATA_READ=1\r\n",
        label,s==EFI_NOT_FOUND?"ABSENT":
        s==EFI_BUFFER_TOO_SMALL?"TOO_LARGE":"ERROR");
    return;
  }
  if(bytes>sizeof(data)) {
    Say("MSI_B850_VARIABLE_RESULT NAME=%s STATE=INVALID_LENGTH\r\n",label);
    return;
  }
  if(bytes==0) {
    Say("MSI_B850_VARIABLE_RESULT NAME=%s STATE=EMPTY\r\n",label);
    return;
  }
  if((attrs & EFI_VARIABLE_BOOTSERVICE_ACCESS)==0)
    Say("MSI_B850_VARIABLE_WARNING NAME=%s BOOTSERVICE_ATTR_MISSING=1\r\n",label);
  if(name[0]=='R' && bytes!=0x147U)
    Say("MSI_B850_VARIABLE_WARNING NAME=%s EXPECTED_BYTES=327 ACTUAL_BYTES=%zu RAW_ONLY=1\r\n",
        label,(size_t)bytes);
  for(off=0;off<bytes;off+=16U) {
    size_t j,count=(size_t)(bytes-off);
    char hex[33];
    if(count>16U)count=16U;
    for(j=0;j<count;j++) {
      UINT8 val=data[off+j];
      hex[2*j]=digits[val>>4];
      hex[2*j+1]=digits[val&0xfU];
    }
    hex[count*2]=0;
    Say("MSI_B850_VARIABLE_HEX NAME=%s OFFSET=%04x DATA=%s\r\n",
        label,(unsigned)off,hex);
  }
  Say("MSI_B850_VARIABLE_RESULT NAME=%s STATE=CAPTURED BYTES=%zu AUTHENTICATED=0\r\n",
      label,(size_t)bytes);
}

/* Original NVIDIA EFI private-controller GUID used in IMG00/IMG01.
 * Diagnostic never invokes an interface callback and never dereferences
 * the opaque interface pointer; an interface is not quiescence consent. */
static EFI_GUID MsiB850NvidiaControllerGuid =
  {0xfe47349a,0x7f0d,0x4641,{0x82,0x2b,0x34,0xba,0xa2,0x8e,0xcd,0xd0}};

static void ReportMsiB850PrivateBindings(EFI_BOOT_SERVICES *bs) {
  size_t i;
  Say("MSI_B850_PRIVATE_BEGIN SCOPE=EXACT_TWO_NVIDIA READ_ONLY=1 NO_VENDOR_CALLBACK=1\r\n");
  for(i=0;i<Snapshot.pci_count;i++) {
    const GU_TOPO_PCI *node=&Snapshot.pci[i];
    EFI_HANDLE h=(EFI_HANDLE)node->identity.handle;
    EFI_PCI_IO_PROTOCOL *pci=(EFI_PCI_IO_PROTOCOL *)node->identity.pci;
    EFI_OPEN_PROTOCOL_INFORMATION_ENTRY *opens=NULL;
    VOID *private_interface=NULL;
    EFI_STATUS ps,os;
    UINTN count=0,j;
    const char *gpu;
    if(!GuMsiB850IsOwnerNvidia(&node->identity))continue;
    gpu=node->identity.pci_id==0x268410deU?"4090":"3080";
    if(!h || !pci) {
      Say("MSI_B850_PRIVATE GPU=%s STATE=SKIPPED REASON=NULL_PCI_HANDLE\r\n",gpu);
      continue;
    }
    ps=bs->HandleProtocol(h,&MsiB850NvidiaControllerGuid,&private_interface);
    Say("MSI_B850_PRIVATE GPU=%s BDF=%04x:%02x:%02x.%u PRIVATE_STATUS=%#llx PRESENT=%u PCI_BY_DRIVER=%u OWNER_AGENT=%p ROM_IMAGE_PRESENT=%u ROM_BYTES=%llu CALLS=ZERO\r\n",
        gpu,(unsigned)node->identity.segment,(unsigned)node->identity.bus,
        (unsigned)node->identity.device,(unsigned)node->identity.function,
        (unsigned long long)ps,(unsigned)(!EFI_ERROR(ps)&&private_interface!=NULL),
        node->by_driver,node->first_owner_agent,
        (unsigned)(pci->RomImage!=NULL),(unsigned long long)pci->RomSize);
    if(!bs->OpenProtocolInformation || !bs->FreePool) {
      Say("MSI_B850_PRIVATE_OPENS GPU=%s STATUS=UNSUPPORTED\r\n",gpu);
      continue;
    }
    os=bs->OpenProtocolInformation(h,&MsiB850NvidiaControllerGuid,&opens,&count);
    Say("MSI_B850_PRIVATE_OPENS GPU=%s STATUS=%#llx COUNT=%llu CAP=%u\r\n",
        gpu,(unsigned long long)os,(unsigned long long)count,
        (unsigned)GU_MSI_B850_PRIVATE_OPEN_LIMIT);
    if(!EFI_ERROR(os)) {
      if(!GuMsiB850PrivateOpensValid((size_t)count,opens)) {
        Say("MSI_B850_PRIVATE_OPENS GPU=%s RESULT=INVALID_OR_OVER_CAP NO_ENTRIES_READ=1\r\n",gpu);
      } else {
        for(j=0;j<count;j++) {
          const EFI_OPEN_PROTOCOL_INFORMATION_ENTRY *entry=&opens[j];
          Say("MSI_B850_PRIVATE_OPENER GPU=%s INDEX=%llu AGENT=%p CONTROLLER=%p ATTRS=%#x OPEN_COUNT=%u AGENT_IS_PCI_OWNER=%u\r\n",
              gpu,(unsigned long long)j,entry->AgentHandle,
              entry->ControllerHandle,(unsigned)entry->Attributes,
              (unsigned)entry->OpenCount,
              (unsigned)(node->by_driver && entry->AgentHandle==node->first_owner_agent));
        }
      }
    }
    if(opens) {
      EFI_STATUS fs=bs->FreePool(opens);
      Say("MSI_B850_PRIVATE_OPENS_FREE GPU=%s STATUS=%#llx\r\n",gpu,(unsigned long long)fs);
    }
  }
  Say("MSI_B850_PRIVATE_END NO_PRIVATE_METHOD=1 NO_ROM_DEREFERENCE=1 NO_RELEASE=1\r\n");
}

/* The loaded image's ParentHandle records the parent supplied to LoadImage.
 * Identify that image without invoking any driver function or altering PCI
 * ownership. This does not prove that the parent called StartImage or
 * enforced a particular per-device option-ROM mode. */
static void SayPath(const char *kind, size_t index, const GU_TOPO_PATH *path);
static int PathCopy(const EFI_DEVICE_PATH_PROTOCOL *source, GU_TOPO_PATH *out);

static void ReportMsiB850ImageParents(EFI_BOOT_SERVICES *bs) {
  size_t i;
  Say("MSI_B850_IMAGE_PARENT_BEGIN READ_ONLY=1 ORIGINAL_LOAD_CALLER_NOT_PROVEN=1\r\n");
  for(i=0;i<Snapshot.pci_count;i++) {
    const GU_TOPO_PCI *node=&Snapshot.pci[i];
    EFI_DRIVER_BINDING_PROTOCOL *binding=NULL;
    EFI_LOADED_IMAGE_PROTOCOL *child=NULL,*parent=NULL;
    EFI_DEVICE_PATH_PROTOCOL *loaded_image_path=NULL;
    EFI_STATUS status;
    GU_TOPO_PATH path;
    const char *gpu;
    if(!GuMsiB850IsOwnerNvidia(&node->identity))continue;
    gpu=node->identity.pci_id==0x268410deU?"4090":"3080";
    if(!node->openers_known || node->by_driver!=1 ||
       !node->first_owner_agent) {
      Say("MSI_B850_IMAGE_PARENT GPU=%s STATUS=UNRESOLVED OWNER_NOT_UNIQUE=1\r\n",gpu);
      continue;
    }
    status=bs->HandleProtocol(node->first_owner_agent,&DriverBindingGuid,
                              (VOID **)&binding);
    if(EFI_ERROR(status)||!binding||!binding->ImageHandle) {
      Say("MSI_B850_IMAGE_PARENT GPU=%s STATUS=UNRESOLVED DRIVER_BINDING_STATUS=%#llx\r\n",
          gpu,(unsigned long long)status);
      continue;
    }
    status=bs->HandleProtocol(binding->ImageHandle,&LoadedGuid,(VOID **)&child);
    if(EFI_ERROR(status)||!child) {
      Say("MSI_B850_IMAGE_PARENT GPU=%s STATUS=UNRESOLVED CHILD_LOADED_STATUS=%#llx\r\n",
          gpu,(unsigned long long)status);
      continue;
    }
    Say("MSI_B850_IMAGE_PARENT GPU=%s CHILD_IMAGE=%p CHILD_SOURCE_IS_PCI=%u PARENT_IMAGE=%p READ_ONLY=1\r\n",
        gpu,binding->ImageHandle,
        (unsigned)(child->DeviceHandle==(EFI_HANDLE)node->identity.handle),
        child->ParentHandle);
    /* This is the exact image-wide DevicePath passed to LoadImage, distinct
     * from LoadedImage.FilePath, which is only relative to DeviceHandle.
     * HandleProtocol is a passive lookup; no call on any NVIDIA interface. */
    status=bs->HandleProtocol(binding->ImageHandle,&LoadedImagePathGuid,
                              (VOID **)&loaded_image_path);
    Say("MSI_B850_LOAD_PATH GPU=%s EFI_STATUS=%#llx PRESENT=%u SOURCE=LOADED_IMAGE_DEVICE_PATH READ_ONLY=1\r\n",
        gpu,(unsigned long long)status,
        (unsigned)(!EFI_ERROR(status)&&loaded_image_path!=NULL));
    if(!EFI_ERROR(status)&&loaded_image_path&&PathCopy(loaded_image_path,&path)) {
      Say("MSI_B850_LOAD_PATH GPU=%s PATH_LEN=%zu\r\n",gpu,path.length);
      SayPath("MSI_B850_LOAD_DEVICE_PATH",i,&path);
    } else {
      Say("MSI_B850_LOAD_PATH GPU=%s PATH_STATUS=UNAVAILABLE_OR_INVALID NO_INFERENCE=1\r\n",gpu);
    }
    if(!child->ParentHandle) {
      Say("MSI_B850_IMAGE_PARENT GPU=%s PARENT_STATUS=ABSENT\r\n",gpu);
      continue;
    }
    status=bs->HandleProtocol(child->ParentHandle,&LoadedGuid,(VOID **)&parent);
    if(EFI_ERROR(status)||!parent) {
      Say("MSI_B850_IMAGE_PARENT GPU=%s PARENT_STATUS=UNRESOLVED EFI_STATUS=%#llx\r\n",
          gpu,(unsigned long long)status);
      continue;
    }
    Say("MSI_B850_IMAGE_PARENT GPU=%s PARENT_STATUS=OBSERVED PARENT_IMAGE_BYTES=%llu PARENT_SOURCE_DEVICE=%p FIRST_BIND_ORDER_UNVERIFIED=1\r\n",
        gpu,(unsigned long long)parent->ImageSize,parent->DeviceHandle);
    if(parent->FilePath && PathCopy(parent->FilePath,&path)) {
      Say("MSI_B850_IMAGE_PARENT GPU=%s PARENT_FILE_PATH_LEN=%zu\r\n",gpu,path.length);
      SayPath("MSI_B850_PARENT_FILE",i,&path);
    } else {
      Say("MSI_B850_IMAGE_PARENT GPU=%s PARENT_FILE_PATH=UNAVAILABLE\r\n",gpu);
    }
    if(parent->ImageBase && parent->ImageSize &&
       parent->ImageSize<=8U*1024U*1024U) {
      size_t rva=0,bytes=0,k;
      if(GuLoadedPeTextRange(parent->ImageBase,(size_t)parent->ImageSize,&rva,&bytes)) {
        UINT8 digest[32];char hex[65];
        static const char digits[]="0123456789abcdef";
        GuSha256((const UINT8 *)parent->ImageBase+rva,bytes,digest);
        for(k=0;k<32;k++) {
          hex[2*k]=digits[digest[k]>>4];
          hex[2*k+1]=digits[digest[k]&15U];
        }
        hex[64]=0;
        Say("MSI_B850_IMAGE_PARENT GPU=%s PARENT_TEXT_SHA256=%s TEXT_RVA=%zu TEXT_BYTES=%zu RELOCATIONS_NOT_CHECKED=1 SIGNATURE_NOT_VERIFIED=1\r\n",
            gpu,hex,rva,bytes);
      } else {
        Say("MSI_B850_IMAGE_PARENT GPU=%s PARENT_TEXT_SHA256=UNAVAILABLE PE_TEXT_UNRESOLVED=1\r\n",gpu);
      }
    } else {
      Say("MSI_B850_IMAGE_PARENT GPU=%s PARENT_TEXT_SHA256=UNAVAILABLE IMAGE_MAPPING_UNAVAILABLE=1\r\n",gpu);
    }
  }
  Say("MSI_B850_IMAGE_PARENT_END NO_VENDOR_CALLBACK=1 NO_PCI_WRITES=1 NO_RELEASE=1\r\n");
}

/* MSI original 1A70 PciBus source: FUN_0000A334 at 0xA46A publishes
 * RDI (the complete private PCI object) as EFI_PCI_IO_PROTOCOL. The pinned
 * source FUN_000065B8 allocates 0x3C8 bytes for kind-5 endpoints and 0xA88
 * for kind-2 bridges; FUN_00003934 reads original policy at object+0x3C0.
 * NEVER assume this layout without all live version, identity, code-owner,
 * memory-span and object-kind checks below. NO callbacks or PCI/MMIO access. */
#define GU_MSI_POLICY_MAP_BYTES 65536U
static UINT64 MsiPolicyMemoryWords[GU_MSI_POLICY_MAP_BYTES/sizeof(UINT64)];
static const UINT8 MsiPciBusTextDigest[32]={0x9b,0xff,0xa5,0xa4,0x57,0xb7,0xd5,0x16,0x01,0xa7,0x3b,0x05,0xf4,0xb0,0xfa,0x66,0xea,0x33,0x81,0xb9,0x81,0xfd,0xe4,0xbd,0x6b,0x88,0x9a,0xb5,0xe3,0x4f,0x59,0xd1};

static int MsiPolicyMapped(const EFI_MEMORY_DESCRIPTOR *map, UINTN map_bytes,
                           UINTN desc_size, const VOID *addr, size_t need) {
  UINTN at;
  UINT64 start=(UINT64)(UINTN)addr;
  if(!map || !addr || !need || desc_size<sizeof(EFI_MEMORY_DESCRIPTOR) ||
     (desc_size%sizeof(UINT64))!=0 || map_bytes%desc_size!=0 ||
     start>UINT64_MAX-(UINT64)need)return 0;
  for(at=0;at<map_bytes;at+=desc_size) {
    const EFI_MEMORY_DESCRIPTOR *d=(const EFI_MEMORY_DESCRIPTOR *)
      ((const UINT8 *)map+at);
    UINT64 physical=(UINT64)d->PhysicalStart;
    UINT64 pages=(UINT64)d->NumberOfPages;
    if(d->Type!=EfiBootServicesData || pages>UINT64_MAX/4096U)continue;
    if(physical>UINT64_MAX-pages*4096U)continue;
    if(start>=physical && start+(UINT64)need<=physical+pages*4096U)
      return 1;
  }
  return 0;
}

static int MsiPolicyTarget(const GU_TOPO_PCI *node,
                           const char **label, UINT32 *kind, size_t *span) {
  const GU_GPU_RECORD *id=&node->identity;
  if(id->segment!=0 || id->function!=0 || id->device!=0)return 0;
  if(id->bus==1 && id->pci_id==0x268410deU && id->class_code==0x0300U) {
    *label="4090";*kind=5;*span=0x3c8U;return 1;
  }
  if(id->bus==4 && id->pci_id==0x220a10deU && id->class_code==0x0300U) {
    *label="3080";*kind=5;*span=0x3c8U;return 1;
  }
  if(id->bus==2 && id->pci_id==0x43f41022U && id->class_code==0x0604U) {
    *label="BRIDGE_02";*kind=2;*span=0xa88U;return 1;
  }
  if(id->bus==3 && id->pci_id==0x43f51022U && id->class_code==0x0604U) {
    *label="BRIDGE_03";*kind=2;*span=0xa88U;return 1;
  }
  return 0;
}

static void ReportMsiB850PolicyPointers(EFI_BOOT_SERVICES *bs) {
  EFI_LOADED_IMAGE_PROTOCOL *parent=NULL;
  EFI_HANDLE parent_handle=NULL;
  EFI_DRIVER_BINDING_PROTOCOL *binding=NULL;
  EFI_LOADED_IMAGE_PROTOCOL *child=NULL;
  EFI_STATUS status;
  EFI_MEMORY_DESCRIPTOR *map=(EFI_MEMORY_DESCRIPTOR *)MsiPolicyMemoryWords;
  UINTN map_size=sizeof(MsiPolicyMemoryWords),key=0,desc_size=0;
  UINT32 desc_version=0;
  size_t i,child_count=0,rva=0,text_bytes=0;
  int parent_ok=1;
  UINT8 digest[32];
  Say("MSI_B850_POLICY_PTR_BEGIN ORIGINAL_PCIBUS_ONLY=1 READ_ONLY=1 NO_ROM_CMN_WRITE=1\r\n");
  if(!bs || !bs->GetMemoryMap || !bs->HandleProtocol) {
    Say("MSI_B850_POLICY_PTR STATE=SKIPPED NO_BOOT_SERVICES=1\r\n");
    goto done;
  }
  for(i=0;i<Snapshot.pci_count;i++) {
    const GU_TOPO_PCI *node=&Snapshot.pci[i];
    if(!GuMsiB850IsOwnerNvidia(&node->identity))continue;
    if(node->by_driver!=1 || !node->openers_known || !node->first_owner_agent) {
      parent_ok=0;break;
    }
    binding=NULL;child=NULL;
    status=bs->HandleProtocol(node->first_owner_agent,&DriverBindingGuid,(VOID **)&binding);
    if(EFI_ERROR(status)||!binding ||
       binding->ImageHandle!=node->first_owner_agent) {parent_ok=0;break;}
    status=bs->HandleProtocol(binding->ImageHandle,&LoadedGuid,(VOID **)&child);
    if(EFI_ERROR(status)||!child ||
       child->DeviceHandle!=(EFI_HANDLE)node->identity.handle ||
       !child->ParentHandle){parent_ok=0;break;}
    if(parent_handle && parent_handle!=child->ParentHandle){parent_ok=0;break;}
    parent_handle=child->ParentHandle;
    child_count++;
  }
  if(!parent_ok || child_count!=2 || !parent_handle) {
    Say("MSI_B850_POLICY_PTR STATE=SKIPPED PARENT_IDENTITY_UNVERIFIED=1\r\n");
    goto done;
  }
  status=bs->HandleProtocol(parent_handle,&LoadedGuid,(VOID **)&parent);
  if(EFI_ERROR(status)||!parent||!parent->ImageBase ||
     parent->ImageSize!=76608U ||
     !GuLoadedPeTextRange(parent->ImageBase,(size_t)parent->ImageSize,
                          &rva,&text_bytes) ||
     rva!=640U || text_bytes!=68716U) {
    Say("MSI_B850_POLICY_PTR STATE=SKIPPED PCIBUS_IMAGE_LAYOUT_UNVERIFIED=1\r\n");
    goto done;
  }
  GuSha256((const UINT8 *)parent->ImageBase+rva,text_bytes,digest);
  if(memcmp(digest,MsiPciBusTextDigest,sizeof(digest))!=0) {
    Say("MSI_B850_POLICY_PTR STATE=SKIPPED PCIBUS_TEXT_HASH_MISMATCH=1\r\n");
    goto done;
  }
  status=bs->GetMemoryMap(&map_size,map,&key,&desc_size,&desc_version);
  Say("MSI_B850_POLICY_PTR MAP_STATUS=%#llx MAP_BYTES=%zu DESC_BYTES=%zu CAP=%u\r\n",
      (unsigned long long)status,(size_t)map_size,(size_t)desc_size,
      (unsigned)sizeof(MsiPolicyMemoryWords));
  if(EFI_ERROR(status)||map_size>sizeof(MsiPolicyMemoryWords) ||
     desc_size<sizeof(EFI_MEMORY_DESCRIPTOR) || desc_size>256U ||
     (desc_size%sizeof(UINT64))!=0 || map_size%desc_size!=0) {
    Say("MSI_B850_POLICY_PTR STATE=SKIPPED MEMORY_MAP_UNVERIFIED=1\r\n");
    goto done;
  }
  for(i=0;i<Snapshot.pci_count;i++) {
    const GU_TOPO_PCI *node=&Snapshot.pci[i];
    EFI_PCI_IO_PROTOCOL *pci=(EFI_PCI_IO_PROTOCOL *)node->identity.pci;
    const char *label=NULL;
    UINT32 expected_kind,kind,config_id;
    size_t object_span;
    const UINT8 *obj;
    VOID *modeptr;
    UINTN method,base=(UINTN)parent->ImageBase;
    UINTN text_start=base+(UINTN)rva;
    UINTN text_end=text_start+(UINTN)text_bytes;
    if(!MsiPolicyTarget(node,&label,&expected_kind,&object_span))continue;
    if(!pci || !MsiPolicyMapped(map,map_size,desc_size,pci,object_span)) {
      Say("MSI_B850_POLICY_PTR TARGET=%s STATE=SKIPPED OBJECT_NOT_IN_BOOT_POOL=1\r\n",label);
      continue;
    }
    /* Original FUN_0000A334 installs the *object base* as PciIo.  Verify the
     * public method lives in the same pinned PciBus .text and the private
     * vendor/device and object kind match this exact PCI controller. */
    method=(UINTN)pci->GetLocation;
    obj=(const UINT8 *)pci;
    memcpy(&config_id,obj+0xe8U,sizeof(config_id));
    memcpy(&kind,obj+0xd0U,sizeof(kind));
    if(method<text_start || method>=text_end ||
       config_id!=node->identity.pci_id || kind!=expected_kind ||
       obj[0xefU]!=(UINT8)(node->identity.class_code>>8)) {
      Say("MSI_B850_POLICY_PTR TARGET=%s STATE=SKIPPED OBJECT_IDENTITY_MISMATCH=1\r\n",label);
      continue;
    }
    memcpy(&modeptr,obj+0x3c0U,sizeof(modeptr));
    if(!modeptr) {
      Say("MSI_B850_POLICY_PTR TARGET=%s STATE=OBSERVED PCI_OBJECT=%p POLICY=ABSENT KIND=%u NO_RUNTIME_MODE=1\r\n",
          label,(VOID *)pci,(unsigned)kind);
      continue;
    }
    if(!MsiPolicyMapped(map,map_size,desc_size,modeptr,0x20U)) {
      Say("MSI_B850_POLICY_PTR TARGET=%s STATE=OBSERVED PCI_OBJECT=%p POLICY=UNREADABLE KIND=%u NO_DEREF=1\r\n",
          label,(VOID *)pci,(unsigned)kind);
      continue;
    }
    {
      const UINT8 *entry=(const UINT8 *)modeptr;
      VOID *record_owner=NULL;
      UINT8 mode=entry[0x18U];
      memcpy(&record_owner,entry+0x10U,sizeof(record_owner));
      Say("MSI_B850_POLICY_PTR TARGET=%s STATE=OBSERVED PCI_OBJECT=%p KIND=%u ENTRY=%p MODE=%u RECORD_OWNER=%p OWNER_IS_SELF=%u ENTRY_FLAGS=%#x ENTRY_CLASS_CACHE=%#x READ_ONLY=1\r\n",
          label,(VOID *)pci,(unsigned)kind,modeptr,(unsigned)mode,record_owner,
          (unsigned)(record_owner==(VOID *)pci),(unsigned)entry[0x19U],
          (unsigned)entry[0x1aU]);
    }
  }
done:
  Say("MSI_B850_POLICY_PTR_END NO_VENDOR_CALLBACK=1 NO_PCI_CONFIG=1 NO_RELEASE=1\r\n");
}

/* The original MSI 1A70 Bds PE has a static AMI Connect Policy at RVA
 * 0x16330 and a list head at RVA 0x163A0. Check its initial/current list
 * state without calling any vendor method or following unknown pointers.
 * This late observation cannot prove historical first-binding order. */
static EFI_GUID MsiBdsConnectGuid =
  {0x38d65ec3,0x8f39,0x4660,{0xb8,0xa6,0xf3,0x6a,0xa3,0x92,0x54,0x75}};
static const UINT8 MsiBdsTextDigest[32]={
  0x19,0x32,0x59,0x15,0x8d,0xf0,0x02,0x81,0x33,0x74,0xb2,0x21,0x9b,0x78,0xc7,0xa6,
  0xc0,0xdc,0x96,0xa0,0x28,0x92,0x26,0x5b,0x40,0x0c,0xeb,0x99,0x69,0x35,0x64,0x8b};

static int MsiBdsExecutableMapped(const EFI_MEMORY_DESCRIPTOR *map,
                                 UINTN bytes, UINTN desc_size,
                                 const VOID *ptr, size_t length) {
  UINTN offset;
  UINT64 addr=(UINT64)(UINTN)ptr;
  if(!map || !ptr || !length ||
     desc_size<sizeof(EFI_MEMORY_DESCRIPTOR) || desc_size>256U ||
     (desc_size%sizeof(UINT64))!=0 || bytes%desc_size!=0 ||
     addr>UINT64_MAX-(UINT64)length)return 0;
  for(offset=0;offset<bytes;offset+=desc_size) {
    const EFI_MEMORY_DESCRIPTOR *d=(const EFI_MEMORY_DESCRIPTOR *)
      ((const UINT8 *)map+offset);
    UINT64 start=(UINT64)d->PhysicalStart,pages=(UINT64)d->NumberOfPages;
    if((d->Type!=EfiBootServicesCode && d->Type!=EfiBootServicesData) ||
       pages>UINT64_MAX/4096U || start>UINT64_MAX-pages*4096U)continue;
    if(addr>=start && addr+(UINT64)length<=start+pages*4096U)return 1;
  }
  return 0;
}

static void ReportMsiB850BdsPolicyState(EFI_BOOT_SERVICES *bs) {
  VOID *policy=NULL;
  EFI_HANDLE *handles=NULL;
  EFI_STATUS status;
  UINTN count=0,i,matches=0,base=0;
  UINTN map_bytes=sizeof(MsiPolicyMemoryWords),map_key=0,desc_size=0;
  UINT32 desc_version=0;
  EFI_MEMORY_DESCRIPTOR *map=(EFI_MEMORY_DESCRIPTOR *)MsiPolicyMemoryWords;
  static const UINTN expected_size=113568U,expected_rva=704U,expected_text=89088U;
  UINT8 digest[32];
  size_t text_rva=0,text_bytes=0;
  UINT64 revision=0;
  VOID *method_get=NULL,*method_set=NULL,*method_class=NULL;
  VOID *head_forward=NULL,*head_backward=NULL;
  const UINT8 *head;
  Say("MSI_B850_BDS_POLICY_BEGIN READ_ONLY=1 VENDOR_CALLS=0 ENTRIES_NOT_WALKED=1\r\n");
  if(!bs || !bs->LocateProtocol || !bs->LocateHandleBuffer ||
     !bs->HandleProtocol || !bs->FreePool || !bs->GetMemoryMap) {
    Say("MSI_B850_BDS_POLICY STATE=SKIPPED BOOT_SERVICES_UNAVAILABLE=1\r\n");
    goto done;
  }
  status=bs->LocateProtocol(&MsiBdsConnectGuid,NULL,&policy);
  Say("MSI_B850_BDS_POLICY LOCATE_STATUS=%#llx PRESENT=%u\r\n",
      (unsigned long long)status,(unsigned)(status==EFI_SUCCESS && policy!=NULL));
  if(EFI_ERROR(status) || !policy) {
    Say("MSI_B850_BDS_POLICY STATE=SKIPPED PROTOCOL_NOT_VERIFIED=1\r\n");
    goto done;
  }
  status=bs->LocateHandleBuffer(ByProtocol,&LoadedGuid,NULL,&count,&handles);
  if(EFI_ERROR(status) || !count || count>1024U || !handles) {
    Say("MSI_B850_BDS_POLICY STATE=SKIPPED LOADED_IMAGES_UNAVAILABLE=1 COUNT=%zu\r\n",
        (size_t)count);
    goto done;
  }
  status=bs->GetMemoryMap(&map_bytes,map,&map_key,&desc_size,&desc_version);
  if(EFI_ERROR(status) || map_bytes>sizeof(MsiPolicyMemoryWords) ||
     desc_size<sizeof(EFI_MEMORY_DESCRIPTOR) || desc_size>256U ||
     (desc_size%sizeof(UINT64))!=0 || (map_bytes%desc_size)!=0) {
    Say("MSI_B850_BDS_POLICY STATE=SKIPPED MEMORY_MAP_UNVERIFIED=1\r\n");
    goto done;
  }
  for(i=0;i<count;i++) {
    EFI_LOADED_IMAGE_PROTOCOL *li=NULL;
    UINTN candidate;
    status=bs->HandleProtocol(handles[i],&LoadedGuid,(VOID **)&li);
    if(EFI_ERROR(status) || !li || !li->ImageBase ||
       li->ImageSize!=(UINT64)expected_size)continue;
    candidate=(UINTN)li->ImageBase;
    if(candidate>(UINTN)-1-expected_size ||
       !MsiBdsExecutableMapped(map,map_bytes,desc_size,li->ImageBase,expected_size) ||
       !GuLoadedPeTextRange(li->ImageBase,(size_t)li->ImageSize,
                            &text_rva,&text_bytes) ||
       text_rva!=(size_t)expected_rva || text_bytes!=(size_t)expected_text)continue;
    GuSha256((const UINT8 *)li->ImageBase+text_rva,text_bytes,digest);
    if(memcmp(digest,MsiBdsTextDigest,sizeof(digest))!=0)continue;
    matches++;
    base=candidate;
  }
  if(matches!=1 || base>(UINTN)-1-0x163b0U ||
     policy!=(VOID *)(base+0x16330U)) {
    Say("MSI_B850_BDS_POLICY STATE=SKIPPED ORIGINAL_BDS_MISMATCH=1 MATCHES=%zu\r\n",
        (size_t)matches);
    goto done;
  }
  memcpy(&revision,policy,sizeof(revision));
  memcpy(&method_get,(const UINT8 *)policy+8U,sizeof(method_get));
  memcpy(&method_set,(const UINT8 *)policy+16U,sizeof(method_set));
  memcpy(&method_class,(const UINT8 *)policy+24U,sizeof(method_class));
  if(revision!=0x10000U || method_get!=(VOID *)(base+0x3260U) ||
     method_set!=(VOID *)(base+0x33e4U) ||
     method_class!=(VOID *)(base+0x34e4U)) {
    Say("MSI_B850_BDS_POLICY STATE=SKIPPED INTERFACE_METHODS_UNVERIFIED=1\r\n");
    goto done;
  }
  head=(const UINT8 *)policy+0x70U;
  memcpy(&head_forward,head,sizeof(head_forward));
  memcpy(&head_backward,head+sizeof(head_forward),sizeof(head_backward));
  Say("MSI_B850_BDS_POLICY STATE=%s HEAD=%p NEXT=%p PREVIOUS=%p CURRENT_BOOT_ONLY=1 EARLY_BINDING_UNPROVEN=1\r\n",
      head_forward==(VOID *)head && head_backward==(VOID *)head ?
      "EMPTY_AT_OBSERVATION":"NONEMPTY_OR_INCONSISTENT",
      (const VOID *)head,head_forward,head_backward);
done:
  if(handles && bs && bs->FreePool) {
    EFI_STATUS freed=bs->FreePool(handles);
    Say("MSI_B850_BDS_POLICY FREE_STATUS=%#llx\r\n",(unsigned long long)freed);
  }
  Say("MSI_B850_BDS_POLICY_END NO_VENDOR_CALLBACK=1 NO_LIST_TRAVERSAL=1 NO_GPU_ACTION=1\r\n");
}

static void ReportMsiB850RomVariables(EFI_SYSTEM_TABLE *table, int pci_ok) {
  Say("MSI_B850_ROM_VARIABLES MODE=READ_ONLY PLATFORM=MS-7E56 CAN_DETACH=0 CAN_UNLOCK=0 CAN_FLASH=0\r\n");
  /* This is the exact MSI 7E56 iGPU and owner dual-NVIDIA PCI fingerprint.
   * It is NOT a signed motherboard/BIOS model certificate. */
  if(!pci_ok || !GuMsiB850KnownRomDiagnosticHardware(&Snapshot)) {
    Say("MSI_B850_ROM_VARIABLES STATUS=SKIPPED PLATFORM_FINGERPRINT=UNVERIFIED GPU_MUTATION=NONE\r\n");
    return;
  }
  Say("MSI_B850_ROM_VARIABLES PLATFORM_FINGERPRINT=MATCH SMBIOS_VERIFIED=0 BIOS_VERSION_VERIFIED=0 GPU_MUTATION=NONE\r\n");
  ReportMsiB850Variable(table,L"ROM_CMN","ROM_CMN");
  ReportMsiB850Variable(table,L"CSM_CMN","CSM_CMN");
  ReportMsiB850PrivateBindings(table->BootServices);
  ReportMsiB850ImageParents(table->BootServices);
  ReportMsiB850PolicyPointers(table->BootServices);
  ReportMsiB850BdsPolicyState(table->BootServices);
  Say("MSI_B850_ROM_VARIABLES STATUS=OBSERVED_OR_REFUSED ORIGINAL_ROM_DISPATCH_UNVERIFIED=1\r\n");
}
#endif

#ifdef GU_TOPO_SELECTION_ENABLED
/* Resolve all requested devices before emitting ordered per-device results.
 * These are sequential CPU-side reports against ONE immutable passive snapshot,
 * never sequential firmware transactions or new GPU hardware reads. */
static int ReportPassiveSelections(int pci_ok, int topology_result) {
  GU_GPU_INVENTORY inventory;
  size_t indices[GU_GPU_SELECTION_COUNT], nodes[GU_GPU_SELECTION_COUNT];
  size_t i;
  int status=GU_INVENTORY_INVALID;
  Say("SELECTION_SEQUENCE=BEGIN REQUESTED=%u READ_ONLY=1 CAN_DETACH=0 CAN_UNLOCK=0 CAN_FLR=0\r\n",
      (unsigned)GU_GPU_SELECTION_COUNT);
  for(i=0;i<GU_GPU_SELECTION_COUNT;i++) {
    const GU_GPU_LOCATION *p=&SelectedLocations[i];
    Say("SELECTION_REQUEST[%zu] BDF=%04x:%02x:%02x.%u\r\n",
        i,(unsigned)p->segment,(unsigned)p->bus,
        (unsigned)p->device,(unsigned)p->function);
  }
  if(pci_ok) {
    status=GuCollectGpuInventory(Snapshot.pci_count,SelectionRead,
                                 &Snapshot,&inventory);
    if(status==GU_INVENTORY_OK)
      status=GuFindGpuSequence(&inventory,SelectedLocations,
                               GU_GPU_SELECTION_COUNT,indices);
    if(status==GU_INVENTORY_OK && Report.gpu_count!=inventory.count)
      status=GU_INVENTORY_NOT_FOUND;
    if(status==GU_INVENTORY_OK)for(i=0;i<GU_GPU_SELECTION_COUNT;i++) {
      const GU_GPU_RECORD *a=&inventory.nvidia[indices[i]];
      size_t node=Report.gpu_index[indices[i]];
      const GU_GPU_RECORD *b;
      if(node>=Snapshot.pci_count) {status=GU_INVENTORY_NOT_FOUND;break;}
      b=&Snapshot.pci[node].identity;
      if(a->handle!=b->handle || a->pci!=b->pci ||
         a->pci_id!=b->pci_id || a->subsystem_id!=b->subsystem_id ||
         a->class_code!=b->class_code ||
         a->segment!=b->segment || a->bus!=b->bus ||
         a->device!=b->device || a->function!=b->function) {
        status=GU_INVENTORY_NOT_FOUND;break;
      }
      nodes[i]=node;
    }
  }
  if(status!=GU_INVENTORY_OK) {
    if(GU_GPU_SELECTION_COUNT==1)
      Say("SELECTION_VERDICT=REFUSED REQUESTED=%04x:%02x:%02x.%u CODE=%d TOPOLOGY_VERDICT=%d READ_ONLY=1\r\n",
          (unsigned)SelectedLocations[0].segment,(unsigned)SelectedLocations[0].bus,
          (unsigned)SelectedLocations[0].device,(unsigned)SelectedLocations[0].function,
          status,topology_result);
    Say("SELECTION_SEQUENCE=REFUSED CODE=%d COMPLETED=0 REQUESTED=%u TOPOLOGY_VERDICT=%d\r\n",
        status,(unsigned)GU_GPU_SELECTION_COUNT,topology_result);
    return 0;
  }
  for(i=0;i<GU_GPU_SELECTION_COUNT;i++) {
    const GU_GPU_RECORD *a=&inventory.nvidia[indices[i]];
    const GU_TOPO_PCI *node=&Snapshot.pci[nodes[i]];
    Say("SELECTION_STEP[%zu]=BEGIN BDF=%04x:%02x:%02x.%u\r\n",
        i,(unsigned)a->segment,(unsigned)a->bus,
        (unsigned)a->device,(unsigned)a->function);
    Say("SELECTION_STEP[%zu]=PASSIVE_IDENTIFIED ID=%04x:%04x SUB=%04x:%04x CLASS=%04x ROOT_INDEX=%zu DEPTH=%u GOP_COUNT=%u BY_DRIVER=%u OPENERS_KNOWN=%u\r\n",
        i,(unsigned)(a->pci_id&65535U),(unsigned)(a->pci_id>>16),
        (unsigned)(a->subsystem_id&65535U),(unsigned)(a->subsystem_id>>16),
        (unsigned)a->class_code,Report.root_index[indices[i]],
        Report.depth[indices[i]],Report.gop_count[indices[i]],
        node->by_driver,(unsigned)node->openers_known);
    Say("SELECTION_STEP[%zu]=END STATUS=OBSERVED READ_ONLY=1\r\n",i);
  }
  if(GU_GPU_SELECTION_COUNT==1) {
    const GU_GPU_RECORD *a=&inventory.nvidia[indices[0]];
    Say("PASSIVE_SELECTION=GPU_IDENTIFIED TARGET=%04x:%02x:%02x.%u ID=%04x:%04x SUB=%04x:%04x CLASS=%04x\r\n",
        (unsigned)a->segment,(unsigned)a->bus,(unsigned)a->device,
        (unsigned)a->function,(unsigned)(a->pci_id&65535U),
        (unsigned)(a->pci_id>>16),(unsigned)(a->subsystem_id&65535U),
        (unsigned)(a->subsystem_id>>16),(unsigned)a->class_code);
    Say("NON_TARGET_NVIDIA_COUNT=%zu\r\n",inventory.count-1);
    Say("SELECTION_VERDICT=PASSIVE_IDENTIFIED REQUESTED=%04x:%02x:%02x.%u CODE=%d TOPOLOGY_VERDICT=%d READ_ONLY=1 CAN_DETACH=0 CAN_UNLOCK=0 CAN_FLR=0\r\n",
        (unsigned)a->segment,(unsigned)a->bus,(unsigned)a->device,
        (unsigned)a->function,GU_INVENTORY_OK,topology_result);
  }
  Say("SELECTION_SEQUENCE=PASSIVE_IDENTIFIED COMPLETED=%u NON_SELECTED_NVIDIA_COUNT=%zu TOPOLOGY_VERDICT=%d CAN_DETACH=0 CAN_UNLOCK=0 CAN_FLR=0\r\n",
      (unsigned)GU_GPU_SELECTION_COUNT,
      inventory.count-GU_GPU_SELECTION_COUNT,topology_result);
  return 1;
}
#endif

/* Emit bounded device-path evidence; lengths alone cannot verify ancestry. */
static void SayPath(const char *kind, size_t index, const GU_TOPO_PATH *path) {
  static const char hex_digits[]="0123456789abcdef";
  size_t at;
  for(at=0;at<path->length;at+=24) {
    char hex[49];
    size_t i,used=path->length-at;
    if(used>24)used=24;
    for(i=0;i<used;i++) {
      hex[2*i]=hex_digits[path->bytes[at+i]>>4];
      hex[2*i+1]=hex_digits[path->bytes[at+i]&15];
    }
    hex[2*used]=0;
    Say("%s[%zu] PATH_OFFSET=%zu PATH_BYTES=%s\r\n",kind,index,at,hex);
  }
}

static int PathCopy(const EFI_DEVICE_PATH_PROTOCOL *source, GU_TOPO_PATH *out) {
  const UINT8 *bytes=(const UINT8 *)source;
  size_t at=0;
  if(!source || !out)return 0;
  memset(out,0,sizeof(*out));
  while(at+4<=GU_TOPO_PATH_BYTES) {
    size_t n=(size_t)bytes[at+2] | ((size_t)bytes[at+3]<<8);
    if(n<4 || n>GU_TOPO_PATH_BYTES-at)return 0;
    if(bytes[at]==0x7f && bytes[at+1]!=0xff)return 0;
    memcpy(out->bytes+at,bytes+at,n);at+=n;
    if(bytes[at-n]==0x7f) {out->length=at;return GuTopoPathValid(out);}
  }
  return 0;
}

/* Configured ConOut and ErrOut are separate UEFI variables. Neither is
 * interchangeable with their *Dev inventories of possible output devices. */
static int ReadConfiguredConsole(EFI_SYSTEM_TABLE *table, const CHAR16 *name,
                                 const char *label, GU_TOPO_PATH *paths,
                                 size_t *count) {
  static UINT8 data[GU_TOPO_PATH_BYTES*GU_TOPO_MAX_CONSOLES];
  UINTN bytes=sizeof(data),at=0,start=0;
  UINT32 attrs=0;EFI_STATUS status;
  *count=0;
  if(!table->RuntimeServices || !table->RuntimeServices->GetVariable) {
    Say("%s_VARIABLE STATUS=UNAVAILABLE CONFIGURED_ONLY=1\r\n",label);
    return 0;
  }
  status=table->RuntimeServices->GetVariable((CHAR16 *)name,&GlobalGuid,&attrs,&bytes,data);
  Say("%s_VARIABLE STATUS=%#llx BYTES=%zu ATTRS=%#x CONFIGURED_ONLY=1\r\n",
      label,(unsigned long long)status,(size_t)bytes,(unsigned)attrs);
  if(EFI_ERROR(status) || !bytes || bytes>sizeof(data))return 0;
  while(at+4<=bytes) {
    size_t n=(size_t)data[at+2] | ((size_t)data[at+3]<<8);
    if(n<4 || n>bytes-at)return 0;
    if(data[at]==0x7f) {
      GU_TOPO_PATH *path;
      if(n!=4 || (data[at+1]!=1 && data[at+1]!=0xff))return 0;
      if(*count>=GU_TOPO_MAX_CONSOLES || at+n-start>GU_TOPO_PATH_BYTES)return 0;
      path=&paths[(*count)++];
      memset(path,0,sizeof(*path));
      memcpy(path->bytes,data+start,at+n-start);
      path->length=at+n-start;
      path->bytes[path->length-3]=0xff; /* normalize END_INSTANCE */
      if(!GuTopoPathValid(path))return 0;
      if(data[at+1]==0xff)return at+n==bytes;
      start=at+n;
    }
    at+=n;
  }
  return 0;
}

/* Independent, observational console inventory. ConOutDev is the list of
 * POSSIBLE output devices; ConsoleOutHandle identifies the *active* simple-
 * text output interface, which may be a virtual/aggregate console handle.
 * Neither report qualifies a physical GOP for detachment. */
static void ReportConsoleBinding(EFI_SYSTEM_TABLE *table) {
  EFI_BOOT_SERVICES *bs=table->BootServices;
  EFI_DEVICE_PATH_PROTOCOL *device_path=NULL;
  EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *out=NULL;
  EFI_STATUS status;
  GU_TOPO_PATH path;
  UINT32 attrs=0;
  UINTN bytes=0,at=0,start=0,index=0;
  static UINT8 data[8192];
  CHAR16 name[]=L"ConOutDev";

  Say("CONSOLE_BINDING_ONLY=1 NOT_DETACH_EVIDENCE=1\r\n");
  Say("CONSOLE_HANDLE ACTIVE=%p ERROR=%p CONOUT_IFACE=%p\r\n",
      table->ConsoleOutHandle,table->StandardErrorHandle,table->ConOut);

  status=table->ConsoleOutHandle ?
      bs->HandleProtocol(table->ConsoleOutHandle,&TextOutGuid,(VOID **)&out) :
      EFI_INVALID_PARAMETER;
  Say("CONSOLE_HANDLE_TEXTOUT STATUS=%#llx IFACE=%p MATCHES_SYSTEM=%u\r\n",
      (unsigned long long)status,out,
      (unsigned)(!EFI_ERROR(status) && out && out==table->ConOut));

  status=table->ConsoleOutHandle ?
      bs->HandleProtocol(table->ConsoleOutHandle,&PathGuid,(VOID **)&device_path) :
      EFI_INVALID_PARAMETER;
  Say("CONSOLE_HANDLE_DEVICE_PATH STATUS=%#llx PATH_PRESENT=%u\r\n",
      (unsigned long long)status,(unsigned)(!EFI_ERROR(status) && !!device_path));
  if(!EFI_ERROR(status) && device_path) {
    if(PathCopy(device_path,&path)) {
      Say("CONSOLE_HANDLE_PATH LEN=%zu\r\n",path.length);
      SayPath("CONSOLE_HANDLE",0,&path);
    } else Say("CONSOLE_HANDLE_PATH INVALID_OR_OVERLONG=1\r\n");
  }

  if(!table->RuntimeServices || !table->RuntimeServices->GetVariable) {
    Say("CONOUTDEV_STATUS=UNAVAILABLE\r\n");return;
  }
  bytes=sizeof(data);
  status=table->RuntimeServices->GetVariable(name,&GlobalGuid,&attrs,&bytes,data);
  Say("CONOUTDEV_VARIABLE STATUS=%#llx BYTES=%zu ATTRS=%#x POSSIBLE_ONLY=1\r\n",
      (unsigned long long)status,(size_t)bytes,(unsigned)attrs);
  if(EFI_ERROR(status) || !bytes || bytes>sizeof(data))return;

  while(at+4<=bytes && index<32) {
    UINTN n=(UINTN)data[at+2] | ((UINTN)data[at+3]<<8);
    if(n<4 || n>bytes-at)break;
    if(data[at]==0x7f) {
      if(n!=4 || (data[at+1]!=1 && data[at+1]!=0xff) ||
         at+n-start>GU_TOPO_PATH_BYTES)break;
      memset(&path,0,sizeof(path));
      memcpy(path.bytes,data+start,at+n-start);
      path.length=at+n-start;
      path.bytes[path.length-3]=0xff;
      if(!GuTopoPathValid(&path))break;
      Say("CONOUTDEV[%zu] PATH_LEN=%zu POSSIBLE_ONLY=1\r\n",
          (size_t)index,path.length);
      SayPath("CONOUTDEV",index,&path);
      index++;
      if(data[at+1]==0xff) {
        Say("CONOUTDEV_PARSE_COMPLETE=%u PATHS=%zu\r\n",
            (unsigned)(at+n==bytes),(size_t)index);
        return;
      }
      start=at+n;
    }
    at+=n;
  }
  Say("CONOUTDEV_PARSE_COMPLETE=0 PATHS=%zu\r\n",(size_t)index);
}

static int PcieRootPort(const UINT32 config[64]) {
  unsigned offset=config[0x34/4]&0xffU,seen=0;
  while(offset>=0x40 && offset<=0xfc && !(offset&3) && seen++<48) {
    UINT32 header=config[offset/4];
    if((header&0xffU)==0x10U)return ((header>>20)&0xfU)==4U;
    offset=(header>>8)&0xffU;
  }
  return 0;
}

/* Every failed PCI read must identify its exact stage in the USB log.
 * This changes error reporting only: no rejected device is skipped. */
static int PciCaptureFailure(const char *stage, EFI_STATUS status) {
  Say("PCI_READ_FAILURE STAGE=%s EFI_STATUS=%#llx\r\n",
      stage,(unsigned long long)status);
  return 0;
}
static int ReadPci(EFI_BOOT_SERVICES *bs, EFI_HANDLE h, GU_TOPO_PCI *node) {
  EFI_PCI_IO_PROTOCOL *io=NULL;
  EFI_DEVICE_PATH_PROTOCOL *path=NULL;
  EFI_OPEN_PROTOCOL_INFORMATION_ENTRY *entries=NULL;
  UINTN count=0,s=0,b=0,d=0,f=0,i;
  UINT32 cfg[64],verify[64],classifier[3],classifier_check[3];
  EFI_STATUS status;
  memset(node,0,sizeof(*node));
  status=bs->HandleProtocol(h,&PciGuid,(VOID **)&io);
  if(EFI_ERROR(status))return PciCaptureFailure("pci-handle-protocol",status);
  if(!io || !io->Pci.Read || !io->GetLocation)
    return PciCaptureFailure("pci-interface-missing",EFI_COMPROMISED_DATA);
  status=io->GetLocation(io,&s,&b,&d,&f);
  if(EFI_ERROR(status))return PciCaptureFailure("pci-get-location",status);
  if(s>65535 || b>255 || d>31 || f>7)
    return PciCaptureFailure("pci-location-invalid",EFI_COMPROMISED_DATA);
  /* Enumerate every PCI protocol handle but include only display functions
   * and PCI-to-PCI bridges in the strict topology graph. Unrelated host
   * bridges can expose vendor-specific, changing config DWORDs.
   * Read only the standard ID/class header for those unrelated functions.
   * A change in classifier identity fails closed instead of hiding a GPU. */
  status=io->Pci.Read(io,EfiPciIoWidthUint32,0,3,classifier);
  if(EFI_ERROR(status))return PciCaptureFailure("pci-classifier-first",status);
  status=io->Pci.Read(io,EfiPciIoWidthUint32,0,3,classifier_check);
  if(EFI_ERROR(status))return PciCaptureFailure("pci-classifier-second",status);
  if(!GuTopoClassifierStable(classifier,classifier_check))
    return PciCaptureFailure("pci-classifier-changed",EFI_COMPROMISED_DATA);
  if(!GuTopoClassRelevant((UINT16)(classifier[2]>>16)))return 2;

  /* Included GPU/bridge functions retain full 256-byte reread stability,
   * BDF/device-path re-resolution and owner-record validation. */
  status=io->Pci.Read(io,EfiPciIoWidthUint32,0,64,cfg);
  if(EFI_ERROR(status))return PciCaptureFailure("pci-config-read",status);
  if(cfg[0]!=classifier[0] || cfg[2]!=classifier[2])
    return PciCaptureFailure("pci-classifier-full-changed",EFI_COMPROMISED_DATA);
  status=bs->HandleProtocol(h,&PathGuid,(VOID **)&path);
  if(EFI_ERROR(status))return PciCaptureFailure("pci-path-protocol",status);
  if(!PathCopy(path,&node->path))
    return PciCaptureFailure("pci-path-invalid",EFI_COMPROMISED_DATA);
  node->identity.handle=h;node->identity.pci=io;
  node->identity.segment=(UINT32)s;node->identity.bus=(UINT32)b;
  node->identity.device=(UINT32)d;node->identity.function=(UINT32)f;
  node->identity.pci_id=cfg[0];node->identity.subsystem_id=cfg[11];
  node->identity.class_code=(UINT16)(cfg[2]>>16);
  node->bridge=node->identity.class_code==0x0604;
  if(node->bridge) {
    node->primary=(UINT8)cfg[0x18/4];
    node->secondary=(UINT8)(cfg[0x18/4]>>8);
    node->subordinate=(UINT8)(cfg[0x18/4]>>16);
    node->root_port=(UINT8)PcieRootPort(cfg);
  }
  /* Only inspect owner records; never Open/Close with BY_DRIVER. */
  if(bs->OpenProtocolInformation) {
    status=bs->OpenProtocolInformation(h,&PciGuid,&entries,&count);
    if(!EFI_ERROR(status)) {
      node->openers_known=1;
      if(count>1024 || (count && !entries)) {
        if(entries && bs->FreePool)(VOID)bs->FreePool(entries);
        return PciCaptureFailure("pci-openers-invalid",EFI_COMPROMISED_DATA);
      }
      for(i=0;i<count;i++) {
        if(entries[i].Attributes & EFI_OPEN_PROTOCOL_BY_DRIVER) {
          if(!node->by_driver)node->first_owner_agent=entries[i].AgentHandle;
          node->by_driver++;
        }
      }
    }
    if(entries) {
      if(!bs->FreePool)
        return PciCaptureFailure("pci-openers-free-unavailable",EFI_UNSUPPORTED);
      status=bs->FreePool(entries);
      if(EFI_ERROR(status))return PciCaptureFailure("pci-openers-free",status);
    }
  }
  /* Fail closed if re-resolved protocols or the *entire* snapshot drift.
   * Do not weaken any stability assertion until the reason is known. */
  {
    EFI_PCI_IO_PROTOCOL *again=NULL;
    EFI_DEVICE_PATH_PROTOCOL *again_path=NULL;
    GU_TOPO_PATH fresh;
    UINTN ss=~(UINTN)0,bb=~(UINTN)0,dd=~(UINTN)0,ff=~(UINTN)0;
    status=bs->HandleProtocol(h,&PciGuid,(VOID **)&again);
    if(EFI_ERROR(status))return PciCaptureFailure("pci-refresh-protocol",status);
    if(again!=io)
      return PciCaptureFailure("pci-refresh-protocol-changed",EFI_COMPROMISED_DATA);
    status=bs->HandleProtocol(h,&PathGuid,(VOID **)&again_path);
    if(EFI_ERROR(status))return PciCaptureFailure("pci-refresh-path-protocol",status);
    if(!PathCopy(again_path,&fresh) || fresh.length!=node->path.length ||
       memcmp(fresh.bytes,node->path.bytes,fresh.length))
      return PciCaptureFailure("pci-refresh-path-changed",EFI_COMPROMISED_DATA);
    status=io->GetLocation(io,&ss,&bb,&dd,&ff);
    if(EFI_ERROR(status))return PciCaptureFailure("pci-refresh-location",status);
    if(ss!=s || bb!=b || dd!=d || ff!=f)
      return PciCaptureFailure("pci-refresh-location-changed",EFI_COMPROMISED_DATA);
    status=io->Pci.Read(io,EfiPciIoWidthUint32,0,64,verify);
    if(EFI_ERROR(status))return PciCaptureFailure("pci-refresh-config-read",status);
    if(memcmp(cfg,verify,sizeof(cfg))) {
      UINTN changed=0,reported=0;
      /* Do not waive drift (including volatile bits) until exact offsets
       * and before/after values have been captured on the real firmware. */
      Say("PCI_CONFIG_DRIFT_BDF=%04x:%02x:%02x.%u ID=%04x:%04x CLASS=%04x\r\n",
          (unsigned)s,(unsigned)b,(unsigned)d,(unsigned)f,
          (unsigned)(cfg[0]&65535U),(unsigned)(cfg[0]>>16),
          (unsigned)(cfg[2]>>16));
      for(i=0;i<64;i++) {
        if(cfg[i]==verify[i])continue;
        changed++;
        if(reported<12) {
          Say("PCI_CONFIG_DRIFT_DWORD OFFSET=%#llx FIRST=%08x SECOND=%08x XOR=%08x\r\n",
              (unsigned long long)(i*4U),(unsigned)cfg[i],(unsigned)verify[i],
              (unsigned)(cfg[i]^verify[i]));
          reported++;
        }
      }
      Say("PCI_CONFIG_DRIFT_SUMMARY CHANGED_DWORDS=%zu REPORTED=%zu TOTAL_DWORDS=64\r\n",
          (size_t)changed,(size_t)reported);
      return PciCaptureFailure("pci-refresh-config-changed",EFI_COMPROMISED_DATA);
    }
  }
  return 1;
}

static int CapturePci(EFI_BOOT_SERVICES *bs) {
  EFI_HANDLE *handles=NULL;
  UINTN count=0,i,skipped=0;
  EFI_STATUS status=bs->LocateHandleBuffer(ByProtocol,&PciGuid,NULL,&count,&handles);
  int okay=1;
  Say("PCI_LOCATE_STATUS=%#llx PCI_HANDLE_COUNT=%zu PCI_LIMIT=%u PCI_NODE_LIMIT=%u\r\n",
      (unsigned long long)status,(size_t)count,(unsigned)GU_TOPO_MAX_HANDLES,
      (unsigned)GU_TOPO_MAX_PCI);
  if(EFI_ERROR(status)) {
    Say("PCI_ENUM_FAILURE STAGE=locate-handles\r\n");okay=0;
  } else if(count==0) {
    Say("PCI_ENUM_FAILURE STAGE=zero-handles\r\n");okay=0;
  } else if(count>GU_TOPO_MAX_HANDLES) {
    Say("PCI_ENUM_FAILURE STAGE=handle-capacity\r\n");okay=0;
  } else if(!handles) {
    Say("PCI_ENUM_FAILURE STAGE=missing-handle-array\r\n");okay=0;
  }
  if(okay)for(i=0;i<count;i++) {
    GU_TOPO_PCI candidate;
    int collected=ReadPci(bs,handles[i],&candidate);
    if(!collected) {
      Say("PCI_ENUM_FAILURE STAGE=device-read INDEX=%zu HANDLE=%p\r\n",
          (size_t)i,handles[i]);
      okay=0;break;
    }
    if(collected==2)skipped++;
    else if(Snapshot.pci_count==GU_TOPO_MAX_PCI) {
      Say("PCI_ENUM_FAILURE STAGE=relevant-node-capacity INDEX=%zu\r\n",(size_t)i);
      okay=0;break;
    } else Snapshot.pci[Snapshot.pci_count++]=candidate;
  }
  Say("PCI_TOPOLOGY_SCOPE=DISPLAY_AND_PCI_BRIDGES INCLUDED=%zu EXCLUDED=%zu TOTAL_PROTOCOL_HANDLES=%zu STRICT_INCLUDED_FULL_256=1\r\n",
      Snapshot.pci_count,(size_t)skipped,(size_t)count);
  if(handles) {
    if(!bs->FreePool) {
      Say("PCI_ENUM_FAILURE STAGE=handles-free-unavailable\r\n");okay=0;
    } else {
      status=bs->FreePool(handles);
      if(EFI_ERROR(status)) {
        Say("PCI_ENUM_FAILURE STAGE=handles-free EFI_STATUS=%#llx\r\n",
            (unsigned long long)status);okay=0;
      }
    }
  }
  return okay;
}

/* Adapt the bounded AllHandles/PCI reporting approach from
 * Windows-GPU-UNLOCK internal-boot/boot/diagnostic.c. This supplementary
 * inventory is strictly diagnostic. Never feed it to GuTopoAssess or GuInit. */
static void ReportPciEvidence(EFI_BOOT_SERVICES *bs) {
  UINTN i;
  UINTN expanded_bridges=0;
  int complete=GuPciObserveReadOnly(bs,&Supplemental);
  Say("PCI_OBSERVATION_ONLY=1 NOT_TOPOLOGY=1 CAN_DETACH=0 CAN_UNLOCK=0\r\n");
  Say("PCI_OBS_SUMMARY COMPLETE=%u ALL_HANDLES=%zu PCI_PROTOCOLS=%zu RELEVANT=%zu RECORDS=%zu ERRORS=%zu TRUNCATED=%u\r\n",
      (unsigned)complete,(size_t)Supplemental.handle_count,
      (size_t)Supplemental.pci_protocol_count,(size_t)Supplemental.relevant_count,
      (size_t)Supplemental.record_count,(size_t)Supplemental.failures,
      (unsigned)Supplemental.truncated);
  Say("PCI_OBS_STATUS LOCATE=%#llx FREE=%#llx FIRST_STAGE=%u FIRST_INDEX=%zu FIRST_STATUS=%#llx\r\n",
      (unsigned long long)Supplemental.locate_status,
      (unsigned long long)Supplemental.free_status,
      (unsigned)Supplemental.first_failure_stage,
      (size_t)Supplemental.first_failure_index,
      (unsigned long long)Supplemental.first_failure_status);
  for(i=0;i<Supplemental.record_count;i++) {
    const GU_PCI_OBS_RECORD *p=&Supplemental.records[i];
    UINT32 id=p->config[0];
    UINT32 class_code=p->config[2]>>16;
    int bridge=class_code==0x0604U;
    int display=(class_code>>8)==0x03U;
    int details=display || (bridge && expanded_bridges++<16);
    Say("PCI_OBS[%zu] ALL_INDEX=%zu %04x:%02x:%02x.%u ID=%04x:%04x SUB=%04x:%04x CLASS=%04x BY_DRIVER=%zu OWNERS_KNOWN=%u OWNER_AGENT=%p PATH_OK=%u PATH_LEN=%zu\r\n",
        (size_t)i,(size_t)p->index,(unsigned)p->segment,(unsigned)p->bus,
        (unsigned)p->device,(unsigned)p->function,
        (unsigned)(id&65535U),(unsigned)(id>>16),
        (unsigned)(p->config[11]&65535U),(unsigned)(p->config[11]>>16),
        (unsigned)class_code,(size_t)p->by_driver,
        (unsigned)p->owners_known,p->first_agent,
        (unsigned)p->path_ok,(size_t)p->path_bytes);
    if(bridge) {
      UINT32 window=p->config[6];
      Say("PCI_OBS_BRIDGE[%zu] PRIMARY=%02x SECONDARY=%02x SUBORDINATE=%02x\r\n",
          (size_t)i,(unsigned)(window&255U),
          (unsigned)((window>>8)&255U),(unsigned)((window>>16)&255U));
    }
    if(details) {
      GU_TOPO_PATH path;
      Say("PCI_OBS_CFG[%zu] %08x %08x %08x %08x %08x %08x %08x %08x\r\n",
          (size_t)i,p->config[0],p->config[1],p->config[2],p->config[3],
          p->config[4],p->config[5],p->config[6],p->config[7]);
      Say("PCI_OBS_CFG[%zu] %08x %08x %08x %08x %08x %08x %08x %08x\r\n",
          (size_t)i,p->config[8],p->config[9],p->config[10],p->config[11],
          p->config[12],p->config[13],p->config[14],p->config[15]);
      if(p->path_ok) {
        memset(&path,0,sizeof(path));
        path.length=(size_t)p->path_bytes;
        memcpy(path.bytes,p->path,p->path_bytes);
        SayPath("PCI_OBS",i,&path);
      }
    }
  }
  Say("PCI_OBSERVATION_NOT_ACCEPTANCE=1\r\n");
}

/* Query current-boot NVIDIA PCI BY_DRIVER agents without invoking any
 * driver callback or changing ownership. This is NOT Stop/recovery evidence. */
static void ReportNvidiaDriverOwners(EFI_BOOT_SERVICES *bs) {
  size_t i;
  Say("NVIDIA_DRIVER_OWNER_INVENTORY=READ_ONLY IMAGE_PROVENANCE_UNVERIFIED=1 NOT_STOP_EVIDENCE=1\r\n");
  for(i=0;i<Snapshot.pci_count;i++) {
    const GU_TOPO_PCI *node=&Snapshot.pci[i];
    const GU_GPU_RECORD *id=&node->identity;
    EFI_DRIVER_BINDING_PROTOCOL *binding=NULL;
    EFI_LOADED_IMAGE_PROTOCOL *loaded=NULL;
    EFI_DEVICE_PATH_PROTOCOL *device_path=NULL;
    EFI_STATUS status;
    GU_TOPO_PATH path;
    if((id->pci_id&0xffffU)!=0x10deU ||
       (id->class_code&0xff00U)!=0x0300U)continue;
    Say("NVIDIA_DRIVER_OWNER[%zu] BDF=%04x:%02x:%02x.%u BY_DRIVER=%u OPENERS_KNOWN=%u AGENT=%p\r\n",
        i,(unsigned)id->segment,(unsigned)id->bus,(unsigned)id->device,
        (unsigned)id->function,node->by_driver,
        (unsigned)node->openers_known,node->first_owner_agent);
    if(!node->openers_known || node->by_driver!=1 || !node->first_owner_agent) {
      Say("NVIDIA_DRIVER_OWNER[%zu] IMAGE_LINK=UNRESOLVED OWNER_NOT_UNIQUE_OR_UNKNOWN=1\r\n",i);
      continue;
    }
    status=bs->HandleProtocol(node->first_owner_agent,&DriverBindingGuid,(VOID **)&binding);
    Say("NVIDIA_DRIVER_OWNER[%zu] BINDING_STATUS=%#llx BINDING_PRESENT=%u\r\n",
        i,(unsigned long long)status,(unsigned)(!EFI_ERROR(status)&&!!binding));
    if(EFI_ERROR(status) || !binding || !binding->ImageHandle) {
      Say("NVIDIA_DRIVER_OWNER[%zu] IMAGE_LINK=UNRESOLVED\r\n",i);
      continue;
    }
    Say("NVIDIA_DRIVER_OWNER[%zu] IMAGE_HANDLE=%p DRIVER_BINDING_HANDLE=%p VERSION=%u\r\n",
        i,binding->ImageHandle,binding->DriverBindingHandle,(unsigned)binding->Version);
    status=bs->HandleProtocol(binding->ImageHandle,&LoadedGuid,(VOID **)&loaded);
    Say("NVIDIA_DRIVER_OWNER[%zu] LOADED_IMAGE_STATUS=%#llx LOADED_IMAGE_PRESENT=%u\r\n",
        i,(unsigned long long)status,(unsigned)(!EFI_ERROR(status)&&!!loaded));
    if(EFI_ERROR(status) || !loaded) {
      Say("NVIDIA_DRIVER_OWNER[%zu] IMAGE_LINK=UNRESOLVED\r\n",i);
      continue;
    }
    Say("NVIDIA_DRIVER_OWNER[%zu] IMAGE_LINK=OBSERVED_ONLY IMAGE_BYTES=%llu SOURCE_DEVICE=%p\r\n",
        i,(unsigned long long)loaded->ImageSize,loaded->DeviceHandle);
    /* SHA-256 of the mapped/possibly relocated in-memory image, NOT a
     * signed file digest, Authenticode decision or proof of GPU ROM origin.
     * Only the bounded LoadedImage-provided mapping is read. */
    if(loaded->ImageBase && loaded->ImageSize &&
       loaded->ImageSize<=8U*1024U*1024U) {
      UINT8 digest[32];
      char hex[65];
      static const char digits[]="0123456789abcdef";
      size_t k;
      GuSha256(loaded->ImageBase,(UINTN)loaded->ImageSize,digest);
      for(k=0;k<32;k++) {
        hex[2*k]=digits[digest[k]>>4];
        hex[2*k+1]=digits[digest[k]&15U];
      }
      hex[64]=0;
      Say("NVIDIA_DRIVER_OWNER[%zu] MAPPED_IMAGE_SHA256=%s RELOCATED_POSSIBLE=1 SIGNATURE_NOT_VERIFIED=1\r\n",i,hex);
    } else {
      Say("NVIDIA_DRIVER_OWNER[%zu] MAPPED_IMAGE_SHA256=UNAVAILABLE IMAGE_SIZE_OUT_OF_BOUNDS_OR_BASE_MISSING=1 SIGNATURE_NOT_VERIFIED=1\r\n",i);
    }
    if(loaded->ImageBase && loaded->ImageSize &&
       loaded->ImageSize<=8U*1024U*1024U) {
      size_t rva=0,bytes=0,k;
      if(GuLoadedPeTextRange(loaded->ImageBase,(size_t)loaded->ImageSize,&rva,&bytes)) {
        UINT8 digest[32];char hex[65];
        static const char digits[]="0123456789abcdef";
        GuSha256((const UINT8 *)loaded->ImageBase+rva,bytes,digest);
        for(k=0;k<32;k++) {
          hex[2*k]=digits[digest[k]>>4];
          hex[2*k+1]=digits[digest[k]&15U];
        }
        hex[64]=0;
        Say("NVIDIA_DRIVER_OWNER[%zu] MAPPED_TEXT_SHA256=%s TEXT_RVA=%zu TEXT_BYTES=%zu TEXT_RELOCATIONS_NOT_CHECKED=1 SIGNATURE_NOT_VERIFIED=1\r\n",
            i,hex,rva,bytes);
      } else Say("NVIDIA_DRIVER_OWNER[%zu] MAPPED_TEXT_SHA256=UNAVAILABLE PE32_PLUS_TEXT_UNRESOLVED=1 SIGNATURE_NOT_VERIFIED=1\r\n",i);
    } else Say("NVIDIA_DRIVER_OWNER[%zu] MAPPED_TEXT_SHA256=UNAVAILABLE IMAGE_MAPPING_UNAVAILABLE=1 SIGNATURE_NOT_VERIFIED=1\r\n",i);
    if(loaded->FilePath && PathCopy(loaded->FilePath,&path)) {
      Say("NVIDIA_DRIVER_OWNER[%zu] FILE_PATH_LEN=%zu\r\n",i,path.length);
      SayPath("NVIDIA_OWNER_FILE",i,&path);
    } else Say("NVIDIA_DRIVER_OWNER[%zu] FILE_PATH=ABSENT_OR_UNVERIFIED\r\n",i);
    if(!loaded->DeviceHandle) {
      Say("NVIDIA_DRIVER_OWNER[%zu] SOURCE_PATH=ABSENT\r\n",i);
      continue;
    }
    status=bs->HandleProtocol(loaded->DeviceHandle,&PathGuid,(VOID **)&device_path);
    Say("NVIDIA_DRIVER_OWNER[%zu] SOURCE_PATH_STATUS=%#llx\r\n",
        i,(unsigned long long)status);
    if(!EFI_ERROR(status) && device_path && PathCopy(device_path,&path)) {
      Say("NVIDIA_DRIVER_OWNER[%zu] SOURCE_PATH_LEN=%zu\r\n",i,path.length);
      SayPath("NVIDIA_OWNER_SOURCE",i,&path);
    } else Say("NVIDIA_DRIVER_OWNER[%zu] SOURCE_PATH=ABSENT_OR_UNVERIFIED\r\n",i);
  }
}


/* Observe at most eight unique loaded GOP/TextOut consumer PE .text mappings.
 * No driver callbacks, controller ownership changes, or GPU operations.
 * A mapped code SHA is evidence, not signature/running-firmware attestation. */
static EFI_HANDLE SharedConsoleImageSeen[8];
static size_t SharedConsoleImageSeenCount;

static void ReportSharedConsoleMappedText(EFI_HANDLE image_handle,
                                          const EFI_LOADED_IMAGE_PROTOCOL *loaded,
                                          const char *source) {
  size_t i,rva=0,bytes=0,j;
  UINT8 digest[32];
  char hex[65];
  static const char digits[]="0123456789abcdef";
  if(!image_handle || !loaded) {
    Say("SHARED_CONSOLE_TEXT SOURCE=%s STATUS=UNKNOWN MISSING_IMAGE=1 NO_STOP_EVIDENCE=1\r\n",
        source);
    return;
  }
  for(i=0;i<SharedConsoleImageSeenCount;i++)
    if(SharedConsoleImageSeen[i]==image_handle)return;
  if(SharedConsoleImageSeenCount>=8U) {
    Say("SHARED_CONSOLE_TEXT SOURCE=%s STATUS=UNKNOWN IMAGE_LIMIT=8 NO_STOP_EVIDENCE=1\r\n",
        source);
    return;
  }
  i=SharedConsoleImageSeenCount++;
  SharedConsoleImageSeen[i]=image_handle;
  if(!loaded->ImageBase || !loaded->ImageSize ||
     loaded->ImageSize>8U*1024U*1024U ||
     !GuLoadedPeTextRange(loaded->ImageBase,(size_t)loaded->ImageSize,
                          &rva,&bytes)) {
    Say("SHARED_CONSOLE_TEXT[%zu] SOURCE=%s IMAGE_HANDLE=%p IMAGE_BYTES=%llu STATUS=UNKNOWN MAPPED_PE_TEXT_UNRESOLVED=1 NO_STOP_EVIDENCE=1\r\n",
        i,source,image_handle,(unsigned long long)loaded->ImageSize);
    return;
  }
  GuSha256((const UINT8 *)loaded->ImageBase+rva,bytes,digest);
  for(j=0;j<32;j++) {
    hex[2*j]=digits[digest[j]>>4];
    hex[2*j+1]=digits[digest[j]&15U];
  }
  hex[64]=0;
  Say("SHARED_CONSOLE_TEXT[%zu] SOURCE=%s IMAGE_HANDLE=%p IMAGE_BYTES=%llu TEXT_RVA=%zu TEXT_BYTES=%zu MAPPED_TEXT_SHA256=%s RELOCATIONS_NOT_CHECKED=1 SIGNATURE_NOT_VERIFIED=1 STOP_NOT_CALLED=1\r\n",
      i,source,image_handle,(unsigned long long)loaded->ImageSize,
      rva,bytes,hex);
}

/* Resolve the GOP protocol's BY_DRIVER agent as a separate owner.
 * It need not be the NVIDIA PCI BY_DRIVER agent; absence of a binding is
 * UNKNOWN, not proof of no consumer. No callback or disconnect is invoked. */
static int ReportGopOpenerAgent(EFI_BOOT_SERVICES *bs,
                                size_t pci, size_t gop, size_t edge,
                                EFI_HANDLE agent) {
  EFI_DRIVER_BINDING_PROTOCOL *binding=NULL;
  EFI_LOADED_IMAGE_PROTOCOL *loaded=NULL;
  EFI_DEVICE_PATH_PROTOCOL *source=NULL;
  GU_TOPO_PATH path;
  EFI_STATUS status;
  if(!agent) {
    Say("NVIDIA_GOP_AGENT[%zu,%zu,%zu] STATUS=UNKNOWN NULL_AGENT=1\r\n",pci,gop,edge);
    return 0;
  }
  status=bs->HandleProtocol(agent,&DriverBindingGuid,(VOID **)&binding);
  Say("NVIDIA_GOP_AGENT[%zu,%zu,%zu] AGENT=%p BINDING_STATUS=%#llx BINDING_PRESENT=%u\r\n",
      pci,gop,edge,agent,(unsigned long long)status,
      (unsigned)(!EFI_ERROR(status)&&!!binding));
  if(EFI_ERROR(status)||!binding||!binding->ImageHandle) {
    Say("NVIDIA_GOP_AGENT[%zu,%zu,%zu] STATUS=UNRESOLVED_DRIVER_BINDING\r\n",pci,gop,edge);
    return 0;
  }
  Say("NVIDIA_GOP_AGENT[%zu,%zu,%zu] IMAGE_HANDLE=%p DRIVER_BINDING_HANDLE=%p VERSION=%u\r\n",
      pci,gop,edge,binding->ImageHandle,binding->DriverBindingHandle,
      (unsigned)binding->Version);
  status=bs->HandleProtocol(binding->ImageHandle,&LoadedGuid,(VOID **)&loaded);
  Say("NVIDIA_GOP_AGENT[%zu,%zu,%zu] LOADED_IMAGE_STATUS=%#llx LOADED_IMAGE_PRESENT=%u\r\n",
      pci,gop,edge,(unsigned long long)status,
      (unsigned)(!EFI_ERROR(status)&&!!loaded));
  if(EFI_ERROR(status)||!loaded) {
    Say("NVIDIA_GOP_AGENT[%zu,%zu,%zu] STATUS=UNRESOLVED_LOADED_IMAGE\r\n",pci,gop,edge);
    return 0;
  }
  Say("NVIDIA_GOP_AGENT[%zu,%zu,%zu] IMAGE_BYTES=%llu SOURCE_DEVICE=%p SIGNATURE_NOT_VERIFIED=1\r\n",
      pci,gop,edge,(unsigned long long)loaded->ImageSize,loaded->DeviceHandle);
  ReportSharedConsoleMappedText(binding->ImageHandle,loaded,"GOP_CONSUMER");
  if(loaded->FilePath&&PathCopy(loaded->FilePath,&path)) {
    Say("NVIDIA_GOP_AGENT[%zu,%zu,%zu] FILE_PATH_LEN=%zu\r\n",pci,gop,edge,path.length);
    SayPath("GOP_AGENT_FILE",gop,&path);
  } else Say("NVIDIA_GOP_AGENT[%zu,%zu,%zu] FILE_PATH=UNAVAILABLE\r\n",pci,gop,edge);
  if(loaded->DeviceHandle) {
    status=bs->HandleProtocol(loaded->DeviceHandle,&PathGuid,(VOID **)&source);
    if(!EFI_ERROR(status)&&source&&PathCopy(source,&path)) {
      Say("NVIDIA_GOP_AGENT[%zu,%zu,%zu] SOURCE_PATH_LEN=%zu\r\n",pci,gop,edge,path.length);
      SayPath("GOP_AGENT_SOURCE",gop,&path);
    } else Say("NVIDIA_GOP_AGENT[%zu,%zu,%zu] SOURCE_PATH=UNAVAILABLE STATUS=%#llx\r\n",
               pci,gop,edge,(unsigned long long)status);
  } else Say("NVIDIA_GOP_AGENT[%zu,%zu,%zu] SOURCE_PATH=UNAVAILABLE\r\n",pci,gop,edge);
  Say("NVIDIA_GOP_AGENT[%zu,%zu,%zu] STATUS=OBSERVED_IMAGE_ONLY NOT_STOP_EVIDENCE=1\r\n",pci,gop,edge);
  return 1;
}


/* A GOP consumer (e.g. GraphicsConsole) may expose SimpleTextOut either on
 * the GOP handle or on a boot-local ControllerHandle seen in a GOP open record.
 * Inspect both separately; this is not a BY_CHILD_CONTROLLER assertion.
 * Observational only: no DriverBinding callback and no protocol ownership
 * changes. Never use boot-local handle addresses in an admission policy. */
static void ReportTextOutputOpeners(EFI_BOOT_SERVICES *bs,const char *scope,
                                    size_t pci,size_t gop,size_t source_edge,
                                    EFI_HANDLE handle,const char *source) {
  EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *text=NULL;
  EFI_OPEN_PROTOCOL_INFORMATION_ENTRY *entries=NULL;
  EFI_STATUS status,release;
  UINTN count=0,k;
  size_t driver_edges=0,unresolved=0;
  if(!handle) {
    Say("%s_TEXTOUT[%zu,%zu,%zu] SOURCE=%s HANDLE=0 STATUS=ABSENT\r\n",
         scope,pci,gop,source_edge,source);
    return;
  }
  status=bs->HandleProtocol(handle,&TextOutGuid,(VOID **)&text);
  Say("%s_TEXTOUT[%zu,%zu,%zu] SOURCE=%s HANDLE=%p TEXTOUT_STATUS=%#llx PRESENT=%u NOT_CHILD_EVIDENCE=1\r\n",
         scope,pci,gop,source_edge,source,handle,(unsigned long long)status,
      (unsigned)(!EFI_ERROR(status)&&!!text));
  if(EFI_ERROR(status)||!text)return;
  if(!bs->OpenProtocolInformation||!bs->FreePool) {
    Say("%s_TEXTOUT[%zu,%zu,%zu] OPENERS_STATUS=UNKNOWN SERVICES_UNAVAILABLE=1\r\n",
         scope,pci,gop,source_edge);
    return;
  }
  status=bs->OpenProtocolInformation(handle,&TextOutGuid,&entries,&count);
  if(EFI_ERROR(status)||count>256U||(count&&!entries)) {
    Say("%s_TEXTOUT[%zu,%zu,%zu] OPENERS_STATUS=UNKNOWN_OR_OVERFLOW EFI_STATUS=%#llx COUNT=%zu\r\n",
         scope,pci,gop,source_edge,(unsigned long long)status,(size_t)count);
    if(entries) {
      release=bs->FreePool(entries);
      Say("%s_TEXTOUT[%zu,%zu,%zu] FREE_STATUS=%#llx\r\n",
         scope,pci,gop,source_edge,(unsigned long long)release);
    }
    return;
  }
  Say("%s_TEXTOUT[%zu,%zu,%zu] OPENERS_STATUS=OBSERVED COUNT=%zu\r\n",
         scope,pci,gop,source_edge,(size_t)count);
  for(k=0;k<count;k++) {
    const EFI_OPEN_PROTOCOL_INFORMATION_ENTRY *e=&entries[k];
    EFI_DRIVER_BINDING_PROTOCOL *binding=NULL;
    EFI_LOADED_IMAGE_PROTOCOL *loaded=NULL;
    GU_TOPO_PATH path;
    EFI_STATUS bstat,lstat;
    Say("%s_TEXTOUT_EDGE[%zu,%zu,%zu,%zu] ATTR=%#x COUNT=%u AGENT=%p CONTROLLER=%p BY_DRIVER=%u BY_CHILD=%u\r\n",
         scope,pci,gop,source_edge,(size_t)k,(unsigned)e->Attributes,
        (unsigned)e->OpenCount,e->AgentHandle,e->ControllerHandle,
        (unsigned)!!(e->Attributes&EFI_OPEN_PROTOCOL_BY_DRIVER),
        (unsigned)!!(e->Attributes&EFI_OPEN_PROTOCOL_BY_CHILD_CONTROLLER));
    if(!(e->Attributes&EFI_OPEN_PROTOCOL_BY_DRIVER))continue;
    driver_edges++;
    if(!e->AgentHandle) {unresolved++;continue;}
    bstat=bs->HandleProtocol(e->AgentHandle,&DriverBindingGuid,(VOID **)&binding);
    if(EFI_ERROR(bstat)||!binding||!binding->ImageHandle) {
      unresolved++;
      Say("%s_TEXTOUT_OWNER[%zu,%zu,%zu,%zu] BINDING_STATUS=%#llx RESOLUTION=UNKNOWN\r\n",
         scope,pci,gop,source_edge,(size_t)k,(unsigned long long)bstat);
      continue;
    }
    lstat=bs->HandleProtocol(binding->ImageHandle,&LoadedGuid,(VOID **)&loaded);
    Say("%s_TEXTOUT_OWNER[%zu,%zu,%zu,%zu] BINDING_STATUS=%#llx LOADED_STATUS=%#llx IMAGE_HANDLE=%p\r\n",
         scope,pci,gop,source_edge,(size_t)k,(unsigned long long)bstat,
        (unsigned long long)lstat,binding->ImageHandle);
    if(EFI_ERROR(lstat)||!loaded) {unresolved++;continue;}
    Say("%s_TEXTOUT_OWNER[%zu,%zu,%zu,%zu] IMAGE_BYTES=%llu SIGNATURE_NOT_VERIFIED=1\r\n",
         scope,pci,gop,source_edge,(size_t)k,(unsigned long long)loaded->ImageSize);
    ReportSharedConsoleMappedText(binding->ImageHandle,loaded,"TEXTOUT_CONSUMER");
    if(loaded->FilePath&&PathCopy(loaded->FilePath,&path)) {
      Say("%s_TEXTOUT_OWNER[%zu,%zu,%zu,%zu] FILE_PATH_LEN=%zu\r\n",
         scope,pci,gop,source_edge,(size_t)k,path.length);
      SayPath("TEXTOUT_OWNER_FILE",k,&path);
    } else {
      Say("%s_TEXTOUT_OWNER[%zu,%zu,%zu,%zu] FILE_PATH=UNKNOWN\r\n",
         scope,pci,gop,source_edge,(size_t)k);
    }
  }
  release=entries?bs->FreePool(entries):EFI_SUCCESS;
  Say("%s_TEXTOUT_SUMMARY[%zu,%zu,%zu] DRIVER_EDGES=%zu UNRESOLVED=%zu FREE_STATUS=%#llx STATUS=%s NO_STOP_EVIDENCE=1\r\n",
         scope,pci,gop,source_edge,driver_edges,unresolved,(unsigned long long)release,
      (!unresolved&&!EFI_ERROR(release))?"OBSERVED_ONLY":"PARTIAL_OR_UNKNOWN");
}

/* Read the current PCI controller opener graph. A BY_CHILD_CONTROLLER
 * record points at its child handle; matching an observed GOP is evidence,
 * not proof of private driver state, Stop semantics, or detach eligibility.
 * Every EFI allocation is bounded and explicitly released. */
static void ReportNvidiaGopGraph(EFI_BOOT_SERVICES *bs) {
  size_t i;
  if(!bs->OpenProtocolInformation || !bs->FreePool) {
    Say("NVIDIA_GOP_GRAPH=UNAVAILABLE NO_OPEN_PROTOCOL_INFORMATION=1\r\n");
    return;
  }
  Say("NVIDIA_GOP_GRAPH=READ_ONLY NO_DRIVER_CALLBACKS=1 NO_DETACH=1\r\n");
  for(i=0;i<Snapshot.pci_count;i++) {
    const GU_TOPO_PCI *p=&Snapshot.pci[i];
    const GU_GPU_RECORD *id=&p->identity;
    EFI_OPEN_PROTOCOL_INFORMATION_ENTRY *edges=NULL;
    EFI_STATUS status,release;
    UINTN count=0,j;
    size_t by_driver=0,by_child=0,gop_children=0,matched=0;
    int complete=1;
    if((id->pci_id&0xffffU)!=0x10deU ||
       (id->class_code&0xff00U)!=0x0300U)continue;
    status=bs->OpenProtocolInformation(id->handle,&PciGuid,&edges,&count);
    if(EFI_ERROR(status) || count>256U || (count && !edges)) {
      Say("NVIDIA_GOP_GRAPH[%zu] PCI_OPEN_STATUS=%#llx OPENERS=%zu GRAPH=UNKNOWN_OR_OVERFLOW\r\n",
          i,(unsigned long long)status,(size_t)count);
      if(edges) {
        release=bs->FreePool(edges);
        Say("NVIDIA_GOP_GRAPH[%zu] FREE_STATUS=%#llx\r\n",i,
            (unsigned long long)release);
      }
      continue;
    }
    Say("NVIDIA_GOP_GRAPH[%zu] PCI_OPEN_STATUS=%#llx OPENERS=%zu\r\n",
        i,(unsigned long long)status,(size_t)count);
    for(j=0;j<count;j++) {
      const EFI_OPEN_PROTOCOL_INFORMATION_ENTRY *e=&edges[j];
      unsigned is_driver=(unsigned)!!(e->Attributes&EFI_OPEN_PROTOCOL_BY_DRIVER);
      unsigned is_child=(unsigned)!!(e->Attributes&EFI_OPEN_PROTOCOL_BY_CHILD_CONTROLLER);
      unsigned same_agent=(unsigned)(e->AgentHandle==p->first_owner_agent);
      size_t k;
      if(is_driver)by_driver++;
      if(is_child)by_child++;
      Say("NVIDIA_PCI_EDGE[%zu,%zu] ATTR=%#x OPEN_COUNT=%u AGENT=%p CHILD=%p DRIVER=%u CHILD_EDGE=%u SAME_AGENT=%u\r\n",
          i,(size_t)j,(unsigned)e->Attributes,(unsigned)e->OpenCount,
          e->AgentHandle,e->ControllerHandle,is_driver,is_child,same_agent);
      if(!is_child)continue;
      if(!e->ControllerHandle) {complete=0;continue;}
      {
        EFI_GRAPHICS_OUTPUT_PROTOCOL *gop=NULL;
        EFI_DEVICE_PATH_PROTOCOL *path=NULL;
        GU_TOPO_PATH copied;
        EFI_STATUS gs=bs->HandleProtocol(e->ControllerHandle,&GopGuid,(VOID **)&gop);
        EFI_STATUS ps=bs->HandleProtocol(e->ControllerHandle,&PathGuid,(VOID **)&path);
        unsigned path_matches=(unsigned)(!EFI_ERROR(ps) && path &&
             PathCopy(path,&copied) && GuTopoPathOnController(&p->path,&copied));
        unsigned handle_matches=0;
        if(!EFI_ERROR(gs) && gop)gop_children++;
        for(k=0;k<Snapshot.gop_count;k++)
          if(GopHandles[k]==e->ControllerHandle)handle_matches++;
        if(handle_matches && path_matches)matched++;
        if((!EFI_ERROR(gs) && gop) && !path_matches)complete=0;
        Say("NVIDIA_PCI_CHILD[%zu,%zu] GOP=%u PATH_ON_CONTROLLER=%u IN_CAPTURED_GOPS=%u GOP_STATUS=%#llx PATH_STATUS=%#llx\r\n",
            i,(size_t)j,(unsigned)(!EFI_ERROR(gs)&&!!gop),
            path_matches,handle_matches,(unsigned long long)gs,(unsigned long long)ps);
      }
    }
    release=edges?bs->FreePool(edges):EFI_SUCCESS;
    if(EFI_ERROR(release))complete=0;
    if(by_driver!=p->by_driver)complete=0;
    Say("NVIDIA_GOP_GRAPH[%zu] PCI_DRIVER_OPENS=%zu EXPECTED=%u CHILD_EDGES=%zu GOP_CHILDREN=%zu MATCHED_GOP_CHILDREN=%zu FREE_STATUS=%#llx GRAPH_STATUS=%s\r\n",
        i,by_driver,p->by_driver,by_child,gop_children,matched,
        (unsigned long long)release,complete?"OBSERVED_ONLY":"PARTIAL_OR_DRIFT");
    /* Enumerate each mapped GOP handle's own protocol openers; empty lists
     * are valid observations, not evidence that NVIDIA Stop is reversible. */
    for(j=0;j<Snapshot.gop_count;j++) {
      EFI_OPEN_PROTOCOL_INFORMATION_ENTRY *opens=NULL;
      EFI_STATUS os,freed;
      UINTN n=0,k;
      size_t by_driver_opens=0,unresolved_agents=0;
      if(!GopHandles[j] ||
         !GuTopoPathOnController(&p->path,&Snapshot.gop[j].path))continue;
      os=bs->OpenProtocolInformation(GopHandles[j],&GopGuid,&opens,&n);
      if(EFI_ERROR(os) || n>256U || (n && !opens)) {
        Say("NVIDIA_GOP_OPENERS[%zu,%zu] STATUS=%#llx COUNT=%zu UNKNOWN_OR_OVERFLOW=1\r\n",
            i,(size_t)j,(unsigned long long)os,(size_t)n);
      } else {
        Say("NVIDIA_GOP_OPENERS[%zu,%zu] STATUS=0 COUNT=%zu\r\n",
            i,(size_t)j,(size_t)n);
        for(k=0;k<n;k++) {
          const EFI_OPEN_PROTOCOL_INFORMATION_ENTRY *e=&opens[k];
          Say("NVIDIA_GOP_EDGE[%zu,%zu,%zu] ATTR=%#x COUNT=%u AGENT=%p CHILD=%p SAME_PCI_DRIVER=%u CONTROLLER_IS_CHILD_ONLY_IF_BY_CHILD=1\r\n",
              i,(size_t)j,(size_t)k,(unsigned)e->Attributes,
              (unsigned)e->OpenCount,e->AgentHandle,e->ControllerHandle,
              (unsigned)(e->AgentHandle==p->first_owner_agent));
          if(e->Attributes&EFI_OPEN_PROTOCOL_BY_DRIVER) {
            by_driver_opens++;
            if(!ReportGopOpenerAgent(bs,i,j,k,e->AgentHandle))
              unresolved_agents++;
          }
          if(e->Attributes&EFI_OPEN_PROTOCOL_BY_DRIVER && e->ControllerHandle)
            ReportTextOutputOpeners(bs,"NVIDIA",i,j,k,e->ControllerHandle,"GOP_OPEN_CONTROLLER");
        }
      }
      /* The actual GOP handle and its open-record ControllerHandle are
       * independent candidates; neither is inferred to be a child. */
      ReportTextOutputOpeners(bs,"NVIDIA",i,j,256U,GopHandles[j],"GOP_HANDLE");
      freed=opens?bs->FreePool(opens):EFI_SUCCESS;
      Say("NVIDIA_GOP_OPENERS_SUMMARY[%zu,%zu] BY_DRIVER=%zu UNRESOLVED_AGENT_IMAGES=%zu OPEN_STATUS=%#llx FREE_STATUS=%#llx STATUS=%s\r\n",
          i,(size_t)j,by_driver_opens,unresolved_agents,
          (unsigned long long)os,(unsigned long long)freed,
          !EFI_ERROR(os)&&n<=256U&&(!n||opens)&&
          !EFI_ERROR(freed)&&!unresolved_agents?"OBSERVED_ONLY":"PARTIAL_OR_UNKNOWN");
      if(EFI_ERROR(freed))
        Say("NVIDIA_GOP_OPENERS[%zu,%zu] FREE_STATUS=%#llx INCOMPLETE=1\r\n",
            i,(size_t)j,(unsigned long long)freed);
    }
  }
}


/* The NVIDIA-specific opener walk cannot establish other display owners.
 * Inspect already captured non-NVIDIA display GOPs without changing protocol
 * ownership, calling drivers, or performing GPU hardware transactions.
 * A missing/ambiguous path is UNKNOWN rather than evidence of safe Stop. */
static void ReportIndependentGopGraph(EFI_BOOT_SERVICES *bs) {
  size_t j,observed=0,unresolved=0;
  if(!bs->OpenProtocolInformation||!bs->FreePool) {
    Say("INDEPENDENT_GOP_GRAPH=UNKNOWN SERVICES_UNAVAILABLE=1 NO_STOP_EVIDENCE=1\r\n");
    return;
  }
  Say("INDEPENDENT_GOP_GRAPH=READ_ONLY NO_DRIVER_CALLBACKS=1 NO_DETACH=1\r\n");
  for(j=0;j<Snapshot.gop_count;j++) {
    size_t i,match=0,pci=0,seen_count=0,k,foreign=0;
    EFI_HANDLE seen[257];
    EFI_OPEN_PROTOCOL_INFORMATION_ENTRY *opens=NULL;
    EFI_STATUS status,freed;
    UINTN count=0;
    size_t by_driver=0;
    /* Match the whole captured PCI graph, not a fixed BDF or handle value. */
    for(i=0;i<Snapshot.pci_count;i++) {
      const GU_TOPO_PCI *p=&Snapshot.pci[i];
      unsigned vendor=(unsigned)(p->identity.pci_id&0xffffU);
      if((p->identity.class_code&0xff00U)!=0x0300U)continue;
      if(!GuTopoPathOnController(&p->path,&Snapshot.gop[j].path))
        continue;
      if(vendor==0x10deU)foreign++;
      else {match++;pci=i;}
    }
    if(!match && foreign)continue; /* NVIDIA GOP handled separately. */
    if(match!=1 || foreign || !GopHandles[j]) {
      Say("INDEPENDENT_GOP[%zu] PCI_MATCHES=%zu NVIDIA_MATCHES=%zu STATUS=AMBIGUOUS_OR_MISSING NO_STOP_EVIDENCE=1\r\n",
          j,match,foreign);
      unresolved++;
      continue;
    }
    observed++;
    Say("INDEPENDENT_GOP[%zu] PCI_INDEX=%zu GOP_HANDLE=%p PCI_MATCHES=1 STATUS=PATH_MATCHED_ONLY\r\n",
        j,pci,GopHandles[j]);
    status=bs->OpenProtocolInformation(GopHandles[j],&GopGuid,&opens,&count);
    if(EFI_ERROR(status)||count>256U||(count&&!opens)) {
      Say("INDEPENDENT_GOP[%zu] GOP_OPEN_STATUS=%#llx COUNT=%zu STATUS=UNKNOWN_OR_OVERFLOW\r\n",
          j,(unsigned long long)status,(size_t)count);
      if(opens) {
        freed=bs->FreePool(opens);
        Say("INDEPENDENT_GOP[%zu] FREE_STATUS=%#llx\r\n",
            j,(unsigned long long)freed);
      }
      unresolved++;
      continue;
    }
    Say("INDEPENDENT_GOP[%zu] GOP_OPEN_STATUS=0 COUNT=%zu\r\n",
        j,(size_t)count);
    /* Do not report a duplicate TextOut opener graph for identical handles. */
    seen[seen_count++]=GopHandles[j];
    ReportTextOutputOpeners(bs,"INDEPENDENT",pci,j,256U,GopHandles[j],
                            "GOP_HANDLE");
    for(k=0;k<count;k++) {
      const EFI_OPEN_PROTOCOL_INFORMATION_ENTRY *e=&opens[k];
      EFI_HANDLE candidate=e->ControllerHandle;
      size_t t;
      unsigned driver=(unsigned)!!(e->Attributes&EFI_OPEN_PROTOCOL_BY_DRIVER);
      if(driver)by_driver++;
      Say("INDEPENDENT_GOP_EDGE[%zu,%zu] PCI_INDEX=%zu ATTR=%#x COUNT=%u AGENT=%p CONTROLLER=%p BY_DRIVER=%u BY_CHILD=%u\r\n",
          j,k,pci,(unsigned)e->Attributes,(unsigned)e->OpenCount,
          e->AgentHandle,candidate,driver,
          (unsigned)!!(e->Attributes&EFI_OPEN_PROTOCOL_BY_CHILD_CONTROLLER));
      if(!driver || !candidate)continue;
      for(t=0;t<seen_count;t++)if(seen[t]==candidate)break;
      if(t!=seen_count) {
        Say("INDEPENDENT_TEXTOUT_ALIAS[%zu,%zu] HANDLE=%p PREVIOUSLY_INSPECTED=1\r\n",
            j,k,candidate);
        continue;
      }
      seen[seen_count++]=candidate;
      ReportTextOutputOpeners(bs,"INDEPENDENT",pci,j,k,candidate,
                              "GOP_OPEN_CONTROLLER");
    }
    freed=opens?bs->FreePool(opens):EFI_SUCCESS;
    if(EFI_ERROR(freed)||!by_driver)unresolved++;
    Say("INDEPENDENT_GOP[%zu] DRIVER_EDGES=%zu DISTINCT_TEXTOUT_HANDLES=%zu FREE_STATUS=%#llx STATUS=%s NO_STOP_EVIDENCE=1\r\n",
        j,by_driver,seen_count,(unsigned long long)freed,
        (EFI_ERROR(freed)||!by_driver)?"PARTIAL_OR_UNKNOWN":"OBSERVED_ONLY");
  }
  Say("INDEPENDENT_GOP_GRAPH_SUMMARY GOP_PATH_MATCHED=%zu UNRESOLVED=%zu STATUS=%s NO_STOP_EVIDENCE=1\r\n",
      observed,unresolved,!observed?"NO_NON_NVIDIA_GOP_OBSERVED":
      unresolved?"PARTIAL_OR_UNKNOWN":"OBSERVED_ONLY");
}

static int CaptureGop(EFI_BOOT_SERVICES *bs) {
  EFI_HANDLE *handles=NULL;
  UINTN count=0,i,j;
  EFI_STATUS status=bs->LocateHandleBuffer(ByProtocol,&GopGuid,NULL,&count,&handles);
  int okay=1;
  if(EFI_ERROR(status)||count>GU_TOPO_MAX_GOPS || (count&&!handles))okay=0;
  if(okay)for(i=0;i<count;i++) {
    EFI_DEVICE_PATH_PROTOCOL *path=NULL;
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop=NULL;
    if(!handles[i] ||
       EFI_ERROR(bs->HandleProtocol(handles[i],&GopGuid,(VOID **)&gop)) || !gop ||
       EFI_ERROR(bs->HandleProtocol(handles[i],&PathGuid,(VOID **)&path)) ||
       !PathCopy(path,&Snapshot.gop[i].path)){okay=0;break;}
    Snapshot.gop[i].handle=handles[i];
    Snapshot.gop[i].protocol=gop;
    GopHandles[Snapshot.gop_count]=handles[i];
    Snapshot.gop_count++;
  }
  if(okay)for(i=0;i<Snapshot.gop_count;i++)
    for(j=0;j<i;j++)
      if(GopHandles[i]==GopHandles[j])okay=0;
  if(handles && (!bs->FreePool || EFI_ERROR(bs->FreePool(handles))))okay=0;
  return okay;
}

/* Re-enumerate all GOP handles after the console reads. The source image
 * remains passive; even a stable GOP set cannot establish Stop safety. */
static int RevalidateGop(EFI_BOOT_SERVICES *bs) {
  EFI_HANDLE *handles=NULL;
  UINTN count=0,i;
  EFI_STATUS status=bs->LocateHandleBuffer(ByProtocol,&GopGuid,NULL,&count,&handles);
  static GU_TOPO_GOP current[GU_TOPO_MAX_GOPS];
  int okay=1;
  if(EFI_ERROR(status) || count!=Snapshot.gop_count ||
     count>GU_TOPO_MAX_GOPS || (count&&!handles))okay=0;
  if(okay)for(i=0;i<count;i++) {
    EFI_DEVICE_PATH_PROTOCOL *path=NULL;
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop=NULL;
    memset(&current[i],0,sizeof(current[i]));
    if(!handles[i] ||
       EFI_ERROR(bs->HandleProtocol(handles[i],&GopGuid,(VOID **)&gop)) || !gop ||
       EFI_ERROR(bs->HandleProtocol(handles[i],&PathGuid,(VOID **)&path)) ||
       !PathCopy(path,&current[i].path)){okay=0;break;}
    current[i].handle=handles[i];
    current[i].protocol=gop;
  }
  if(okay && !GuTopoGopSnapshotsEqual(Snapshot.gop,Snapshot.gop_count,
                                      current,(size_t)count))okay=0;
  if(handles && (!bs->FreePool || EFI_ERROR(bs->FreePool(handles))))okay=0;
  Say("GOP_CAPTURE_RECHECK STABLE=%u ORIGINAL=%zu CURRENT=%zu NON_ATOMIC=1 NO_STOP_EVIDENCE=1\r\n",
      (unsigned)okay,Snapshot.gop_count,(size_t)count);
  return okay;
}

#ifdef GU_TOPO_SAVE_LOG
/* Opt-in diagnostic file on the app's own EFI volume. Never overwrite an
 * earlier capture. The ONLY intended persistent mutation is this new file. */
static EFI_STATUS SaveLog(EFI_HANDLE image, EFI_SYSTEM_TABLE *table) {
  EFI_LOADED_IMAGE_PROTOCOL *li=NULL;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs=NULL;
  EFI_FILE_PROTOCOL *root=NULL,*file=NULL,*prior=NULL;
  EFI_STATUS s;
  UINTN bytes;
  if(LogOverflow || !LogUsed)return EFI_BAD_BUFFER_SIZE;
  s=table->BootServices->HandleProtocol(image,&LoadedGuid,(VOID **)&li);
  if(EFI_ERROR(s)||!li)return EFI_NOT_FOUND;
  s=table->BootServices->HandleProtocol(li->DeviceHandle,&FsGuid,(VOID **)&fs);
  if(EFI_ERROR(s)||!fs||!fs->OpenVolume)return EFI_UNSUPPORTED;
  s=fs->OpenVolume(fs,&root);
  if(EFI_ERROR(s)||!root||!root->Open||!root->Close)return EFI_UNSUPPORTED;
  /* Windows-GPU-UNLOCK style non-overwriting numbered captures: repeated
   * USB boots must not block Windows merely because an older log exists. */
  {
    UINTN attempt;
    CHAR16 name[]=L"\\GPU-TOPOLOGY-DIAGNOSTIC-0000.TXT";
    UINTN chars=0;
    while(name[chars])chars++;
    for(attempt=0;attempt<10000;attempt++) {
      name[chars-8]=(CHAR16)('0'+attempt/1000);
      name[chars-7]=(CHAR16)('0'+(attempt/100)%10);
      name[chars-6]=(CHAR16)('0'+(attempt/10)%10);
      name[chars-5]=(CHAR16)('0'+attempt%10);
      prior=NULL;
      s=root->Open(root,&prior,name,EFI_FILE_MODE_READ,0);
      if(s==EFI_SUCCESS) {
        if(!prior || !prior->Close || EFI_ERROR(prior->Close(prior))) {
          (VOID)root->Close(root);return EFI_DEVICE_ERROR;
        }
        continue;
      }
      if(s!=EFI_NOT_FOUND) { (VOID)root->Close(root);return s; }
      s=root->Open(root,&file,name,
                   EFI_FILE_MODE_READ|EFI_FILE_MODE_WRITE|EFI_FILE_MODE_CREATE,0);
      if(EFI_ERROR(s) || !file || !file->Write || !file->Close) {
        if(file && file->Close)(VOID)file->Close(file);
        (VOID)root->Close(root);return EFI_DEVICE_ERROR;
      }
      Say("LOG_FILE=\\GPU-TOPOLOGY-DIAGNOSTIC-%04u.TXT\r\n",(unsigned)attempt);
      break;
    }
    if(attempt==10000) {(VOID)root->Close(root);return EFI_VOLUME_FULL;}
  }
  bytes=(UINTN)LogUsed;
  s=file->Write(file,&bytes,LogBytes);
  if(!EFI_ERROR(s) && bytes!=(UINTN)LogUsed)s=EFI_DEVICE_ERROR;
  if(!EFI_ERROR(s) && (!file->Flush || EFI_ERROR(file->Flush(file))))s=EFI_DEVICE_ERROR;
  if(EFI_ERROR(file->Close(file)))s=EFI_DEVICE_ERROR;
  if(EFI_ERROR(root->Close(root)))s=EFI_DEVICE_ERROR;
  return s;
}
#endif

EFI_STATUS EFIAPI UefiMain(EFI_HANDLE image, EFI_SYSTEM_TABLE *table) {
  EFI_BOOT_SERVICES *bs;
  int pci_ok,gop_ok,console_ok,error_ok,error_classified,result;
  int console_stable=0,error_stable=0;
  GU_TOPO_CONSOLE_OWNERS error_owners={0};
  static GU_TOPO_PATH error_paths[GU_TOPO_MAX_CONSOLES];
  static GU_TOPO_PATH final_console[GU_TOPO_MAX_CONSOLES];
  static GU_TOPO_PATH final_error[GU_TOPO_MAX_CONSOLES];
  size_t error_count=0,final_console_count=0,final_error_count=0,i;
#ifndef GU_TOPO_SAVE_LOG
  (void)image;
#endif
  if(!table || !table->BootServices || !table->ConOut ||
     !table->BootServices->LocateHandleBuffer ||
     !table->BootServices->HandleProtocol ||
     !table->BootServices->FreePool)return EFI_INVALID_PARAMETER;
  System=table;bs=table->BootServices;
  Say("GPU TOPOLOGY DIAGNOSTIC ONLY - no GPU hardware writes or unlock calls\r\n");
  Say("READ_ONLY=1 CAN_DETACH=0 CAN_UNLOCK=0 CAN_FLR=0\r\n");
  pci_ok=CapturePci(bs);
  if(!pci_ok)ReportPciEvidence(bs);
#ifdef GU_MSI_B850_ROM_VARIABLES
  ReportMsiB850RomVariables(table,pci_ok);
#endif
  gop_ok=CaptureGop(bs);
  console_ok=ReadConfiguredConsole(table,L"ConOut","CONOUT",
                                   Snapshot.console,&Snapshot.console_count);
  Snapshot.console_available=(uint8_t)console_ok;
  error_ok=ReadConfiguredConsole(table,L"ErrOut","ERROUT",error_paths,&error_count);
  ReportConsoleBinding(table);
  /* Re-read both configured variables after separate console queries. A
   * changed or unreadable path inventory cannot justify the final verdict.
   * These sequential observations cannot eliminate later hardware races. */
  console_stable=console_ok &&
      ReadConfiguredConsole(table,L"ConOut","CONOUT_RECHECK",
                            final_console,&final_console_count) &&
      GuTopoConsolePathsEqual(Snapshot.console,Snapshot.console_count,
                              final_console,final_console_count);
  error_stable=error_ok &&
      ReadConfiguredConsole(table,L"ErrOut","ERROUT_RECHECK",
                            final_error,&final_error_count) &&
      GuTopoConsolePathsEqual(error_paths,error_count,
                              final_error,final_error_count);
  Say("CONSOLE_PATHS_RECHECK CONOUT_STABLE=%u ERROUT_STABLE=%u NON_ATOMIC=1\r\n",
      (unsigned)console_stable,(unsigned)error_stable);
  /* A stale GOP census cannot support a final read-only COMPLETE verdict. */
  if(gop_ok)gop_ok=RevalidateGop(bs);
  else Say("GOP_CAPTURE_RECHECK STABLE=0 SKIPPED_INITIAL_CAPTURE_FAILED=1 NON_ATOMIC=1\r\n");
  console_ok=console_stable;
  error_ok=error_stable;
  Snapshot.console_available=(uint8_t)console_ok;
  error_classified=pci_ok && gop_ok && console_ok && error_ok &&
      GuTopoConsoleOwners(&Snapshot,error_paths,error_count,&error_owners);
  Say("ERROR_CONSOLE_CAPTURE=%u CLASSIFIED=%u NVIDIA=%u INDEPENDENT=%u UNMATCHED=%u AMBIGUOUS=%u INDEPENDENT_WITHOUT_GOP=%u NOT_DETACH_EVIDENCE=1\r\n",
      (unsigned)error_ok,(unsigned)error_classified,
      error_owners.nvidia,error_owners.independent,
      error_owners.unmatched,error_owners.ambiguous,
      error_owners.independent_without_gop);
  Say("ERROR_CONSOLE_ISOLATION=%s\r\n",
      !error_classified?"UNKNOWN":
      error_owners.nvidia || error_owners.ambiguous?"CONFLICT":
      error_owners.unmatched || error_owners.independent_without_gop ||
      !error_owners.independent?"INCOMPLETE":"INDEPENDENT_OBSERVED");
  for(i=0;i<error_count;i++) {
    Say("ERROUT[%zu] PATH_LEN=%zu\r\n",i,error_paths[i].length);
    SayPath("ERROUT",i,&error_paths[i]);
  }
  result=(pci_ok && gop_ok)?GuTopoAssess(&Snapshot,&Report):GU_TOPO_INVALID;
  /* Preserve the graph-only result as separately inspectable evidence.
   * The final verdict must also be vetoed by unsafe/unknown ErrOut. */
  Say("GRAPH_ONLY_TOPOLOGY_VERDICT=%s ERROUT_INCLUDED_IN_FINAL=1\r\n",
      result==GU_TOPO_COMPLETE?"COMPLETE":
      result==GU_TOPO_INCOMPLETE?"INCOMPLETE":
      result==GU_TOPO_CONFLICT?"CONFLICT":"INVALID");
  result=GuTopoCombinedConsoleVerdict(result,error_classified,&error_owners);
  Say("PCI_CAPTURE=%u GOP_CAPTURE=%u CONOUT_CAPTURE=%u PCI_NODES=%zu GOP_NODES=%zu CONOUT_PATHS=%zu\r\n",
      (unsigned)pci_ok,(unsigned)gop_ok,(unsigned)console_ok,
      Snapshot.pci_count,Snapshot.gop_count,Snapshot.console_count);
  for(i=0;i<Snapshot.pci_count;i++) {
    const GU_TOPO_PCI *p=&Snapshot.pci[i];
    Say("PCI[%zu] %04x:%02x:%02x.%u %04x:%04x SUB=%04x:%04x CLASS=%04x ROOTPORT=%u PRIMARY=%02x BUSWINDOW=%02x-%02x BY_DRIVER=%u KNOWN=%u AGENT=%p PATH=%zu\r\n",
      i,(unsigned)p->identity.segment,(unsigned)p->identity.bus,
      (unsigned)p->identity.device,(unsigned)p->identity.function,
      (unsigned)(p->identity.pci_id&65535U),(unsigned)(p->identity.pci_id>>16),
      (unsigned)(p->identity.subsystem_id&65535U),(unsigned)(p->identity.subsystem_id>>16),
      (unsigned)p->identity.class_code,(unsigned)p->root_port,
      (unsigned)p->primary,(unsigned)p->secondary,(unsigned)p->subordinate,
      p->by_driver,(unsigned)p->openers_known,p->first_owner_agent,p->path.length);
    SayPath("PCI",i,&p->path);
  }
  for(i=0;i<Snapshot.gop_count;i++) {
    Say("GOP[%zu] PATH_LEN=%zu\r\n",i,Snapshot.gop[i].path.length);
    SayPath("GOP",i,&Snapshot.gop[i].path);
  }
  for(i=0;i<Snapshot.console_count;i++) {
    Say("CONOUT[%zu] PATH_LEN=%zu\r\n",i,Snapshot.console[i].length);
    SayPath("CONOUT",i,&Snapshot.console[i]);
  }
  if(pci_ok && gop_ok) {
    for(i=0;i<Report.gpu_count;i++) {
      const GU_GPU_RECORD *gpu=&Snapshot.pci[Report.gpu_index[i]].identity;
      Say("GPU[%zu] BDF=%04x:%02x:%02x.%u ID=%04x:%04x SUB=%04x:%04x PCI_INDEX=%zu PARENT=%zu ROOT_PORT=%zu DEPTH=%u ANCESTRY=%u ROOT_VALID=%u GOP_COUNT=%u\r\n",
          i,(unsigned)gpu->segment,(unsigned)gpu->bus,
          (unsigned)gpu->device,(unsigned)gpu->function,
          (unsigned)(gpu->pci_id&65535U),(unsigned)(gpu->pci_id>>16),
          (unsigned)(gpu->subsystem_id&65535U),(unsigned)(gpu->subsystem_id>>16),
          Report.gpu_index[i],Report.parent_index[i],Report.root_index[i],
          Report.depth[i],(unsigned)Report.ancestry_verified[i],
          (unsigned)Report.root_port_verified[i],Report.gop_count[i]);
    }
    Say("NVIDIA_GPU_COUNT=%zu INDEPENDENT_GOP_COUNT=%u INDEPENDENT_CONOUT_MATCH=%u NVIDIA_CONOUT_MATCH=%u GOP_UNMATCHED=%u INDEPENDENT_CONSOLE_ASSOCIATION=%u CONOUT_WITHOUT_GOP=%u CONOUT_UNMATCHED=%u CONOUT_AMBIGUOUS=%u\r\n",
        Report.gpu_count,Report.independent_gop_count,
        Report.independent_console_count,Report.nvidia_console_count,
        Report.unmatched_gops,(unsigned)Report.independent_console_observed,
        Report.independent_console_without_gop,Report.unmatched_console_count,
        Report.ambiguous_console_count);
  }
  Say("STATUS=%s TOPOLOGY=%d DISCOVERED_NVIDIA_GPUS=%zu HARDWARE_TESTED=0 PHYSICAL_DISPLAY_OWNER=UNKNOWN\r\n",
      result==GU_TOPO_COMPLETE?"COMPLETE":result==GU_TOPO_INVALID?"ERROR":"PARTIAL",
      result,Report.gpu_count);
  /* Keep legacy STATUS=PARTIAL for existing log consumers, but distinguish
   * an incomplete graph from an explicit topology conflict in the log. */
  Say("TOPOLOGY_VERDICT=%s NVIDIA_CONOUT_BLOCKER=%u\r\n",
      result==GU_TOPO_COMPLETE?"COMPLETE":
      result==GU_TOPO_INCOMPLETE?"INCOMPLETE":
      result==GU_TOPO_CONFLICT?"CONFLICT":"INVALID",
      (unsigned)(Report.nvidia_console_count!=0));
  if(pci_ok) {
    ReportNvidiaDriverOwners(bs);
    if(gop_ok) {
      ReportNvidiaGopGraph(bs);
      ReportIndependentGopGraph(bs);
    } else {
      Say("NVIDIA_GOP_GRAPH=UNAVAILABLE GOP_CAPTURE_FAILED=1\r\n");
      Say("INDEPENDENT_GOP_GRAPH=UNAVAILABLE GOP_CAPTURE_FAILED=1\r\n");
    }
  } else Say("NVIDIA_DRIVER_OWNER_INVENTORY=UNAVAILABLE PCI_CAPTURE_FAILED=1\r\n");
#ifdef GU_TOPO_SELECTION_ENABLED
  SelectionOk=ReportPassiveSelections(pci_ok,result);
#endif
  Say("DIAGNOSTIC FINISHED; GPU hardware untouched.\r\n");
#ifdef GU_TOPO_WINDOWS_HANDOFF
  Say("WINDOWS_HANDOFF=REQUESTED; pinned Windows ESP, no reset/BootOrder write\r\n");
#endif
#ifdef GU_TOPO_SAVE_LOG
  {
    EFI_STATUS written=SaveLog(image,table);
    Say("SAVE_LOG_STATUS=%#llx ROTATING_FILE=GPU-TOPOLOGY-DIAGNOSTIC-NNNN.TXT\r\n",
        (unsigned long long)written);
    if(EFI_ERROR(written))return written;
  }
#endif
#ifdef GU_TOPO_WINDOWS_HANDOFF
  /* A partial passive observation does not change GPU state. Always attempt
   * normal Windows startup after successful durable capture/close. */
  Say("Starting the explicitly pinned Windows Boot Manager directly.\r\n");
  {
    EFI_STATUS handoff=GuWindowsBootManager(image,table);
    Say("WINDOWS_HANDOFF_RETURNED=%#llx; no retry or reset\r\n",
        (unsigned long long)handoff);
    return handoff;
  }
#else
#ifdef GU_TOPO_SELECTION_ENABLED
  return result==GU_TOPO_COMPLETE && SelectionOk?EFI_SUCCESS:EFI_UNSUPPORTED;
#else
  return result==GU_TOPO_COMPLETE?EFI_SUCCESS:EFI_UNSUPPORTED;
#endif
#endif
}
