"""Test the real UEFI driver installer and variable readers without firmware I/O."""
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

SOURCE = (Path(__file__).resolve().parents[1] / "src/driver_nvram.c").read_text(encoding="utf-8")


def production(name):
    match = re.search(
        r"\b(?:static\s+)?(?:EFI_STATUS|BOOLEAN|VOID|UINTN)\s+" +
        re.escape(name) + r"\s*\([^;]*?\)\s*\{", SOURCE)
    if match is None:
        raise AssertionError("Missing function: " + name)
    pos, depth = match.end(), 1
    while pos < len(SOURCE) and depth:
        depth += (SOURCE[pos] == "{") - (SOURCE[pos] == "}")
        pos += 1
    if depth:
        raise AssertionError("Unclosed function: " + name)
    return SOURCE[match.start():pos]


class NvramCapacityTests(unittest.TestCase):
    def test_real_install_refuses_unreadable_driver_order_before_any_write(self):
        code = "\n\n".join(production(name) for name in (
            "GetDevicePathSize", "SetDriverSlot", "IsOwnOption",
            "ScanDriverOptions", "ReadDriverOrder", "GuInstallDriverOption"))
        host = r"""
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>
typedef void VOID;
typedef uint8_t UINT8;
typedef uint16_t UINT16;
typedef uint32_t UINT32;
typedef size_t UINTN;
typedef wchar_t CHAR16;
typedef int EFI_STATUS;
typedef VOID *EFI_HANDLE;
typedef struct {int v;} EFI_GUID;
typedef struct {UINT8 Type,SubType,Length[2];} EFI_DEVICE_PATH_PROTOCOL;
typedef struct {EFI_HANDLE DeviceHandle;} EFI_LOADED_IMAGE_PROTOCOL;
#define BOOLEAN int
#define TRUE 1
#define FALSE 0
#define LOAD_OPTION_ACTIVE 1U
#define GU_DRIVER_SCAN_SLOTS 256U
#define GU_DRIVER_ORDER_MAX 128U
#define GU_DRIVER_VAR_ATTRS 7U
#define EfiBootServicesData 1
#define EFI_SUCCESS 0
#define EFI_NOT_FOUND 1
#define EFI_BUFFER_TOO_SMALL 2
#define EFI_OUT_OF_RESOURCES 3
#define EFI_COMPROMISED_DATA 4
#define EFI_DEVICE_ERROR 5
#define EFI_UNSUPPORTED 6
#define EFI_INVALID_PARAMETER 7
typedef struct EFI_BOOT_SERVICES {
  EFI_STATUS (*HandleProtocol)(EFI_HANDLE,EFI_GUID *,VOID **);
  EFI_STATUS (*AllocatePool)(int,UINTN,VOID **);
  EFI_STATUS (*FreePool)(VOID *);
} EFI_BOOT_SERVICES;
typedef struct EFI_RUNTIME_SERVICES {
  EFI_STATUS (*GetVariable)(CHAR16 *,EFI_GUID *,UINT32 *,UINTN *,VOID *);
  EFI_STATUS (*SetVariable)(CHAR16 *,EFI_GUID *,UINT32,UINTN,VOID *);
} EFI_RUNTIME_SERVICES;
typedef struct {EFI_BOOT_SERVICES *BootServices;
                EFI_RUNTIME_SERVICES *RuntimeServices;} EFI_SYSTEM_TABLE;
static EFI_GUID GlobalGuid,LoadedGuid,PathGuid;
static const CHAR16 DriverDescription[]=L"AdaUnlock";
static const CHAR16 DriverFile[]=L"\\EFI\\Drivers\\AdaUnlockDxe.efi";
static EFI_LOADED_IMAGE_PROTOCOL Loaded={(EFI_HANDLE)(uintptr_t)0x1234};
static UINT8 VolumePath[]={1,1,4,0,0x7f,0xff,4,0};
static int mode,allocs,frees,driver_writes,order_writes,driver_deletes,order_reads;
static EFI_STATUS DriverFilePresent(EFI_BOOT_SERVICES *bs,EFI_HANDLE vol) {
  (void)bs;return vol==Loaded.DeviceHandle?EFI_SUCCESS:EFI_DEVICE_ERROR;
}
static EFI_STATUS HandleProtocol(EFI_HANDLE h,EFI_GUID *guid,VOID **out) {
  (void)h;
  if(guid==&LoadedGuid)*out=&Loaded;
  else if(guid==&PathGuid)*out=VolumePath;
  else return EFI_DEVICE_ERROR;
  return EFI_SUCCESS;
}
static EFI_STATUS Allocate(int type,UINTN n,VOID **out) {
  if(type!=EfiBootServicesData||!out)return EFI_DEVICE_ERROR;
  allocs++;*out=calloc(1,n);
  return *out?EFI_SUCCESS:EFI_OUT_OF_RESOURCES;
}
static EFI_STATUS Free(VOID *p) {frees++;free(p);return EFI_SUCCESS;}
static void BuildOwnOption(UINT8 *buffer,UINTN *size) {
  UINT8 *p=buffer;
  UINT32 attrs=1;
  UINT16 node_len=(UINT16)(4+sizeof(DriverFile));
  UINT16 list_len=(UINT16)(node_len+4);
  memcpy(p,&attrs,4);p+=4;
  memcpy(p,&list_len,2);p+=2;
  memcpy(p,DriverDescription,sizeof(DriverDescription));
  p+=sizeof(DriverDescription);
  p[0]=4;p[1]=4;p[2]=(UINT8)node_len;
  p[3]=(UINT8)(node_len>>8);p+=4;
  memcpy(p,DriverFile,sizeof(DriverFile));p+=sizeof(DriverFile);
  p[0]=0x7f;p[1]=0xff;p[2]=4;p[3]=0;p+=4;
  *size=(UINTN)(p-buffer);
}
static EFI_STATUS GetVariable(CHAR16 *name,EFI_GUID *guid,UINT32 *attrs,
                              UINTN *bytes,VOID *data) {
  (void)guid;(void)attrs;
  if(wcscmp(name,L"DriverOrder")==0) {
    order_reads++;
    if(mode==9)return EFI_DEVICE_ERROR;
    if(mode==4)return EFI_NOT_FOUND;
    UINTN count=(mode==3||mode==7)?127:128;
    if(mode==5){*bytes=258;return EFI_SUCCESS;}
    if(*bytes<count*sizeof(UINT16))return EFI_BUFFER_TOO_SMALL;
    UINT16 *order=(UINT16 *)data;
    for(UINTN i=0;i<count;i++)order[i]=(UINT16)(100+i);
    if(mode==1)order[0]=2;
    *bytes=count*sizeof(UINT16);
    return EFI_SUCCESS;
  }
  if(mode==8 && wcscmp(name,L"Driver0001")==0)return EFI_DEVICE_ERROR;
  if(mode==6 && wcscmp(name,L"Driver0002")==0) {
    *bytes=1025;return EFI_SUCCESS;
  }
  if((mode==1||mode==2) && wcscmp(name,L"Driver0002")==0) {
    if(*bytes<256)return EFI_BUFFER_TOO_SMALL;
    BuildOwnOption((UINT8 *)data,bytes);
    return EFI_SUCCESS;
  }
  return EFI_NOT_FOUND;
}
static EFI_STATUS SetVariable(CHAR16 *name,EFI_GUID *guid,UINT32 attrs,
                              UINTN size,VOID *data) {
  (void)guid;(void)attrs;
  if(wcscmp(name,L"DriverOrder")==0) {
    order_writes++;
    if(!data||size>256)return EFI_DEVICE_ERROR;
    return mode==7?EFI_DEVICE_ERROR:EFI_SUCCESS;
  }
  if(size==0&&!data)driver_deletes++;
  else {driver_writes++;if(!data)return EFI_DEVICE_ERROR;}
  return EFI_SUCCESS;
}
""" + code + r"""
#define CHECK(x) do {if(!(x)){fprintf(stderr,"FAIL mode %d line %d: %s\n",mode,__LINE__,#x);return 1;}}while(0)
int main(int argc,char **argv) {
  EFI_BOOT_SERVICES bs={HandleProtocol,Allocate,Free};
  EFI_RUNTIME_SERVICES rs={GetVariable,SetVariable};
  EFI_SYSTEM_TABLE st={&bs,&rs};
  mode=argc>1?atoi(argv[1]):-1;
  if(mode<0||mode>9)return 2;
  EFI_STATUS result=GuInstallDriverOption((EFI_HANDLE)(uintptr_t)0x55,&st);
  EFI_STATUS wanted=mode==0||mode==2?EFI_OUT_OF_RESOURCES:
                    mode==5||mode==6?EFI_COMPROMISED_DATA:
                    mode==7||mode==8||mode==9?EFI_DEVICE_ERROR:EFI_SUCCESS;
  CHECK(result==wanted);
  CHECK(order_reads==1);
  if(mode==0||mode==2||mode==5||mode==6||mode==8||mode==9) {
    CHECK(allocs==0&&frees==0);
    CHECK(driver_writes==0&&order_writes==0&&driver_deletes==0);
  } else if(mode==1) {
    CHECK(driver_writes==1&&order_writes==0&&driver_deletes==0);
    CHECK(allocs==1&&frees==1);
  } else if(mode==3||mode==4) {
    CHECK(driver_writes==1&&order_writes==1&&driver_deletes==0);
    CHECK(allocs==1&&frees==1);
  } else if(mode==7) {
    CHECK(driver_writes==1&&order_writes==1&&driver_deletes==1);
    CHECK(allocs==1&&frees==1);
  }
  return 0;
}
"""
        with tempfile.TemporaryDirectory() as directory:
            source=Path(directory)/"driver_capacity.c"
            source.write_text(host,encoding="utf-8")
            for compiler in ("gcc","clang"):
                binary=Path(directory)/("driver_capacity_"+compiler)
                built=subprocess.run(
                    [compiler,"-std=c11","-O2","-Wall","-Wextra",
                     "-Werror","-pedantic",str(source),"-o",str(binary)],
                    capture_output=True,text=True)
                self.assertEqual(built.returncode,0,built.stderr)
                for mode in range(10):
                    with self.subTest(compiler=compiler,mode=mode):
                        result=subprocess.run(
                            [str(binary),str(mode)],
                            capture_output=True,text=True)
                        self.assertEqual(result.returncode,0,
                                         result.stdout+result.stderr)


if __name__=="__main__":
    unittest.main()
