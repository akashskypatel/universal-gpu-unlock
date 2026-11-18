/* SPDX-License-Identifier: GPL-2.0-only */
/* Parent-supported Ada, Ampere and Turing reference bundles.
 * A matched manifest is not signed-firmware or hardware qualification. */
#include "profile.h"
#include "checks.h"
#include "sha256.h"
#include "compat/compat.h"
#include "port.h"
#include "prom_identity.h"

static const GU_ARCH_ASSETS AdaAssets={{
    {L"\\AD102-ASSETS\\booter_load_ad102.bin","gpu-unlock/610.43.03/booter_load_ad102.bin","6ee05b20e1dc244c08c052f36ef28469b8d09f4955d1fe33e48a179e11a7fd49",57672U},
    {L"\\AD102-ASSETS\\v67-ad102-610.43.03-four-plm.bin","gpu-unlock/610.43.03/v67-ad102-610.43.03-four-plm.bin","44c36b2d6b3424e53e18f3e55db85c833655de071ce66ce42749588a4a7dc8cb",64000U},
    {L"\\AD102-ASSETS\\gsp_ga10x.bin","gpu-unlock/610.43.03/gsp_ga10x.bin","572373620a37418f24dc16b5031c39338778c3257e48e8408de9a57291b24f3a",84277400U},
    {L"\\AD102-ASSETS\\gsp_boot_ad102.bin","gpu-unlock/610.43.03/gsp_boot_ad102.bin","45d6335a8185d6ff13e8d1484cea73f68d3d13aa1c19dc080ed1c3482dd5bac0",36864U},
    {L"\\AD102-ASSETS\\gsp_boot_desc_ad102.bin","gpu-unlock/610.43.03/gsp_boot_desc_ad102.bin","83f8f3f1f5f9496e0ea8fc7103c2beb64d0b6de2f5ffe725f13514de5a95fa42",84U}
  },L"\\GA102-ASSETS\\gsp_ga10x.bin"};
static const GU_ARCH_ASSETS AmpereAssets={{
    {L"\\GA102-ASSETS\\booter_load_ga102.bin","gpu-unlock/610.43.03/booter_load_ga102.bin","056cbc064a888f80717726c54d7f64fcd68044d123870c95a74f37f4975dcaa4",61256U},
    {L"\\GA102-ASSETS\\v67-ga102-610.43.03.bin","gpu-unlock/610.43.03/v67-ga102-610.43.03.bin","2a0aa1f86d0b6afff5214d641d87d46dc98ef06ed908ff05d0ddbad5ffb7e197",64000U},
    {L"\\GA102-ASSETS\\gsp_ga10x.bin","gpu-unlock/610.43.03/gsp_ga10x.bin","572373620a37418f24dc16b5031c39338778c3257e48e8408de9a57291b24f3a",84277400U},
    {L"\\GA102-ASSETS\\gsp_boot_ga102.bin","gpu-unlock/610.43.03/gsp_boot_ga102.bin","3df651acb174fd5f5561a1996965b192f50c5d763168446ed0d4ead19d895768",24576U},
    {L"\\GA102-ASSETS\\gsp_boot_desc_ga102.bin","gpu-unlock/610.43.03/gsp_boot_desc_ga102.bin","cc5a3fc1f4c47f2fe107b1b68f0176d8a448e6dec84b575e5560ccef5262bf15",84U}
  },L"\\AD102-ASSETS\\gsp_ga10x.bin"};
/* The parent supplies these 610.43.03 reference assets. The fork currently
 * carries no TU102 payload bytes; EFI checks actual files and hashes. */
static const GU_ARCH_ASSETS TuringAssets={{
    {L"\\TU102-ASSETS\\booter_load_tu102.bin","gpu-unlock/610.43.03/booter_load_tu102.bin","3ae2cccc2666e5de1fee0db68a41ba37086124425eaed4d90718e194d5d48276",59976U},
    {L"\\TU102-ASSETS\\v67-tu102-610.43.03.bin","gpu-unlock/610.43.03/v67-tu102-610.43.03.bin","2a0aa1f86d0b6afff5214d641d87d46dc98ef06ed908ff05d0ddbad5ffb7e197",64000U},
    {L"\\TU102-ASSETS\\gsp_tu10x.bin","gpu-unlock/610.43.03/gsp_tu10x.bin","73065619db9ec921d19fc4e519dd04d91a9199b525eaca9b257b89fb8c5ec52c",29352832U},
    {L"\\TU102-ASSETS\\gsp_boot_tu102.bin","gpu-unlock/610.43.03/gsp_boot_tu102.bin","8765694cd38beaa4e2b8b1b83a31fb7aee6cb9d8b7c6bc7a86e6d931a3a056c2",4096U},
    {L"\\TU102-ASSETS\\gsp_boot_desc_tu102.bin","gpu-unlock/610.43.03/gsp_boot_desc_tu102.bin","9e7cca2cfdc5ce601b0adba39967850046108df6c2d23fff95578ca3f0ccbe01",76U}
  },NULL};
const GU_ARCH_ASSETS *GuGetArchAssetsForPciId(UINT32 pci_id) {
  if((pci_id & 0xffffU)!=0x10deU)return NULL;
  UINT16 dev=(UINT16)(pci_id>>16);
  if(GuIsAdaDeviceId(dev))return &AdaAssets;
  if(GuIsAmpereDeviceId(dev))return &AmpereAssets;
  if(GuIsTuringDeviceId(dev))return &TuringAssets;
  return NULL;
}
static const GU_ARCH_ASSETS *PlatformManifest(const GU_PLATFORM *platform) {
  if(!platform)return NULL;
  const GU_ARCH_ASSETS *assets=GuGetArchAssetsForPciId(
      ((UINT32)platform->device_id<<16)|0x10deU);
  if(!assets ||
     (!!platform->is_turing)!=(assets==&TuringAssets) ||
     (!!platform->is_ampere)!=(assets==&AmpereAssets))return NULL;
  return assets;
}
const GU_ARCH_ASSETS *GuGetArchAssets(VOID) {return PlatformManifest(&Gu);}
/* Inspect an explicitly supplied owner, including a retired platform.
 * The loader publishes canonical manifest names. Do not dereference
 * unproved names or firmware pointers and do not change the active slot. */
BOOLEAN GuPlatformAssetsOwned(const GU_PLATFORM *platform) {
  const GU_ARCH_ASSETS *arch=PlatformManifest(platform);
  if(!arch)return FALSE;
  for(UINTN i=0;i<GU_ASSET_COUNT;i++) {
    const GU_ARCH_ASSET *a=&arch->entries[i];
    const GU_ASSET *entry=&platform->asset_entries[i];
    if(entry->name!=a->name||entry->firmware.size!=a->bytes||
       !GuCpuPoolOwns(platform,entry->firmware.data,a->bytes))return FALSE;
    for(UINTN j=0;j<i;j++)
      if(entry->firmware.data==platform->asset_entries[j].firmware.data)return FALSE;
  }
  return TRUE;
}
BOOLEAN GuAd104AssetsValid(const GU_PLATFORM *platform) {
  const GU_ARCH_ASSETS *arch=PlatformManifest(platform);
  if(!arch||!GuPlatformAssetsOwned(platform))return FALSE;
  for(UINTN i=0;i<GU_ASSET_COUNT;i++) {
    const GU_ARCH_ASSET *a=&arch->entries[i];
    if(!GuHashMatches(platform->asset_entries[i].firmware.data,a->bytes,a->sha256))
      return FALSE;
  }
  return TRUE;
}
BOOLEAN GuAd104PromMatches(const GU_PLATFORM *platform,const VOID *Data,UINTN Bytes) {
  /* The live NV_PROM_DATA window is 1 MiB, unlike the accepted 2 MiB
   * archive. Compare live PCI identity and validate the PROM structure;
   * the FWSEC descriptor and cold/PCI snapshots are checked separately.
   * Neither structural validity nor a logged digest authenticates FWSEC. */
  if(!platform)return FALSE;
  if(!GuPromImageMatchesDevice((const unsigned char *)Data,Bytes,platform->device_id)) {
    GuLog("PROM qualification refused: invalid ROM or mismatched live PCI device %04x\n",
          (unsigned)platform->device_id);
    return FALSE;
  }
  UINT8 hash[32];
  GuSha256(Data,Bytes,hash);
  CHAR8 s[65];
  static const char hex[]="0123456789abcdef";
  for(int i=0;i<32;i++){s[i*2]=hex[(hash[i]>>4)&0xf];s[i*2+1]=hex[hash[i]&0xf];}
  s[64]=0;
  GuLog("PROM readout SHA256: %s PCIR_device=%04x STRUCTURE_VALID=1 SIGNATURE_AUTHENTICATED=0\n",
        s,(unsigned)platform->device_id);
  return TRUE;
}
BOOLEAN GuAd104FwsecMatches(const struct gpu_unlock_fwsec *f) {
  if (!f) return FALSE;
  return f->descriptor_version == 3 && f->ucode_id != 0 &&
         f->sig_count >= 1 &&
         f->sig_bytes == (UINT32)f->sig_count * 384;
}

static BOOLEAN ConfigFor(GU_PLATFORM *platform) {
  UINT32 c[64];UINTN s,b,d,f;GU_CAPS caps;
  if(!platform)return FALSE;
  const GU_GPU_RECORD *id=&platform->discovery_identity;
  if(platform!=GuPlatformActive){platform->fault=TRUE;return FALSE;}
  if(!platform->handle||!platform->pci||id->handle!=platform->handle||id->pci!=platform->pci||
     id->pci_id!=(((UINT32)platform->device_id<<16)|0x10deU)||
     !platform->pci->GetLocation||!platform->pci->Pci.Read||
     !platform->bar_base||platform->bar_size!=0x1000000||
     EFI_ERROR(platform->pci->GetLocation(platform->pci,&s,&b,&d,&f))) {
    GuLog("Config: invalid pci handle or location\n");return FALSE;
  }
  if(s!=id->segment||b!=id->bus||d!=id->device||f!=id->function) {
    GuLog("Config: discovery location mismatch\n");return FALSE;
  }
  for(UINTN i=0;i<64;i++) {
    if(EFI_ERROR(GuConfigReadFor(platform,EfiPciIoWidthUint32,(UINT32)i*4,&c[i]))) {
      GuLog("Config: read failed at word %u\n",(unsigned)i);return FALSE;
    }
  }
  BOOLEAN accepted=GuCheckConfig(c,s,b,d,f,&caps);
#ifdef GU_ACTIVE_SINGLE_TARGET
  if(!accepted)accepted=GuCheckConfigInitialMaster(c,s,b,d,f,&caps);
#endif
  if(!accepted) {
    GuLog("Config: cold PCI policy refused (seg=%u bus=%u dev=%u fun=%u)\n",
          (unsigned)s,(unsigned)b,(unsigned)d,(unsigned)f);return FALSE;
  }
  if(c[0]!=id->pci_id||c[11]!=id->subsystem_id||(c[2]>>16)!=id->class_code||
     !GuIsSupportedPciId(c[0])||((c[2]>>16)!=0x0300U&&(c[2]>>16)!=0x0302U)||
     (c[4]&~0xfU)!=platform->bar_base) {
    GuLog("Config: mismatch c0=0x%x c2=0x%x c4=0x%x bar_base=0x%llx\n",
          c[0],c[2],c[4],(unsigned long long)platform->bar_base);return FALSE;
  }
  if(!(caps.pm==platform->pm_cap&&caps.pcie==platform->pcie_cap&&
       caps.pcie_version==platform->pcie_version&&caps.msi==platform->msi_cap&&
       caps.msix==platform->msix_cap)) {
    GuLog("Config: caps mismatch: pm=%u/%u pcie=%u/%u pcie_ver=%u/%u msi=%u/%u msix=%u/%u\n",
          caps.pm,platform->pm_cap,caps.pcie,platform->pcie_cap,
          caps.pcie_version,platform->pcie_version,caps.msi,platform->msi_cap,
          caps.msix,platform->msix_cap);return FALSE;
  }
  GuLog("Config: OK\n");return TRUE;
}
/* The TU10x PMC architecture nibble identifies the Turing cold state.
 * TU102 does not expose Ada/Ampere speed-select registers. This is a
 * passive diagnostic predicate, not a selector or unlock authorization. */
static BOOLEAN TuringColdState(UINT32 pmc, UINT32 wpr2_hi) {
  return pmc!=0xffffffffU &&
         (pmc&0xffff0000U)!=0xbadf0000U &&
         ((pmc>>24)&0x0fU)==0x06U && (wpr2_hi>>4)==0;
}
static BOOLEAN RegistersFor(GU_PLATFORM *platform,UINT32 *Ss0,UINT32 *Ss1) {
  if(!platform||platform!=GuPlatformActive) {
    if(platform)platform->fault=TRUE;
    return FALSE;
  }
  UINT32 pmc=GuRead32For(platform,0);
  if(platform->fault||pmc==0xffffffffU||(pmc&0xffff0000U)==0xbadf0000U) {
    GuLog("Registers: BAR0 PMC read failed (0x%x)\n",pmc);return FALSE;
  }
  UINT32 wpr2_hi=GuRead32For(platform,0x1fa828);
  if(platform->fault||(wpr2_hi>>4)!=0) {
    GuLog("Registers: WPR2 already active (hi=0x%x), cold reboot required\n",wpr2_hi);
    return FALSE;
  }
  if(platform->is_turing) {
    if(!TuringColdState(pmc,wpr2_hi)) {
      GuLog("TURING_COLD=REFUSED PMC=%#x WPR2_HI=%#x\n",pmc,wpr2_hi);return FALSE;
    }
    *Ss0=*Ss1=0;
    GuLog("TURING_COLD=PASSIVE PMC=%#x WPR2_HI=%#x SELECTORS_UNMAPPED=1 ACTIVE_ALLOWED=0\n",
          pmc,wpr2_hi);
    return !platform->fault;
  }
  *Ss0=GuRead32For(platform,0x82381c);*Ss1=GuRead32For(platform,0x823820);
  GuLog("Selectors: SS0=0x%x, SS1=0x%x (fault=%u)\n",
        *Ss0,*Ss1,(unsigned)platform->fault);
  if(*Ss0==0xffffffffU||(*Ss0&0xffff0000U)==0xbadf0000U||
     *Ss0==0x88888888U||*Ss1>7) {
    GuLog("Selectors invalid: SS0=0x%x, SS1=0x%x (expected SS1 <= 7)\n",*Ss0,*Ss1);
    return FALSE;
  }
  GuLog("Registers: cold state verified (PMC=0x%x, SS0=0x%x, SS1=%u)\n",
        pmc,*Ss0,*Ss1);
  return !platform->fault;
}
BOOLEAN GuAd104ColdEntryFor(GU_PLATFORM *platform) {
  UINT32 ss0,ss1;
  return platform&&platform==GuPlatformActive&&!platform->started&&!platform->fault&&
    !platform->retained&&ConfigFor(platform)&&RegistersFor(platform,&ss0,&ss1)&&
    GuEntrySelectorsMatchFor(platform,ss0,ss1);
}
BOOLEAN GuAd104ColdEntry(VOID) {return GuAd104ColdEntryFor(&Gu);}
/* A read-only firmware and selector report; never set qualification latches. */
static BOOLEAN DiagnosticOwnerMatches(GU_PLATFORM *owner,EFI_BOOT_SERVICES *bs,
                                      EFI_PCI_IO_PROTOCOL *pci,EFI_HANDLE handle) {
  if(owner==GuPlatformActive&&owner->bs==bs&&owner->pci==pci&&owner->handle==handle)
    return TRUE;
  /* Never release a diagnostic allocation through a substituted owner. */
  owner->fault=TRUE;owner->retained=TRUE;
  return FALSE;
}
EFI_STATUS GuDiagnosticScan(VOID) {
  GU_PLATFORM *owner=GuPlatformActive;
  if(!owner)return EFI_NOT_READY;
  EFI_BOOT_SERVICES *bs=owner->bs;
  EFI_PCI_IO_PROTOCOL *pci=owner->pci;
  EFI_HANDLE handle=owner->handle;
  UINT32 before0=0,before1=0,after0=0,after1=0;
  struct gpu_unlock_fwsec fw;
  if(owner->started||owner->owned||owner->retained||owner->profile_verified||owner->fault)
    return EFI_NOT_READY;
  BOOLEAN config_ok=ConfigFor(owner);
  if(!DiagnosticOwnerMatches(owner,bs,pci,handle))return EFI_DEVICE_ERROR;
  BOOLEAN regs_ok=config_ok&&RegistersFor(owner,&before0,&before1);
  if(!DiagnosticOwnerMatches(owner,bs,pci,handle))return EFI_DEVICE_ERROR;
  BOOLEAN assets_ok=GuAd104AssetsValid(owner);
  GuLog("DIAGNOSTIC CONFIG=%u REGISTERS=%u PINNED_ASSETS=%u\n",
        (unsigned)config_ok,(unsigned)regs_ok,(unsigned)assets_ok);
  if(!DiagnosticOwnerMatches(owner,bs,pci,handle))return EFI_DEVICE_ERROR;
  if(!config_ok||!regs_ok||!assets_ok)return EFI_NOT_READY;
  VOID *prom=GuAlloc(1048576U);
  if(!DiagnosticOwnerMatches(owner,bs,pci,handle))return EFI_DEVICE_ERROR;
  if(!prom)return EFI_OUT_OF_RESOURCES;
  BOOLEAN copied=GuCopyFromIo(owner,prom,(VOID *)(UINTN)(owner->bar_base+0x300000),1048576U);
  if(!DiagnosticOwnerMatches(owner,bs,pci,handle))return EFI_DEVICE_ERROR;
  BOOLEAN prom_ok=copied&&!owner->fault&&GuAd104PromMatches(owner,prom,1048576U);
  int parsed=prom_ok?gpu_unlock_vbios_fwsec(prom,1048576U,&fw):-1;
  BOOLEAN fw_ok=parsed==0&&GuAd104FwsecMatches(&fw);
  if(!DiagnosticOwnerMatches(owner,bs,pci,handle))return EFI_DEVICE_ERROR;
  BOOLEAN cfg2=ConfigFor(owner);
  if(!DiagnosticOwnerMatches(owner,bs,pci,handle))return EFI_DEVICE_ERROR;
  BOOLEAN reg2=cfg2&&RegistersFor(owner,&after0,&after1);
  if(!DiagnosticOwnerMatches(owner,bs,pci,handle))return EFI_DEVICE_ERROR;
  BOOLEAN stable=cfg2&&reg2&&before0==after0&&before1==after1;
  GuLog("DIAGNOSTIC PROM=%u FWSEC_PARSE=%d FWSEC_MATCH=%u STABLE=%u SIGNATURE_AUTHENTICATED=0\n",
        (unsigned)prom_ok,parsed,(unsigned)fw_ok,(unsigned)stable);
  if(!DiagnosticOwnerMatches(owner,bs,pci,handle))return EFI_DEVICE_ERROR;
  GuFree(prom);
  if(!DiagnosticOwnerMatches(owner,bs,pci,handle)||owner->fault||owner->retained)return EFI_DEVICE_ERROR;
  if(parsed==0 && fw.descriptor_version==2)
    return EFI_UNSUPPORTED; /* V2 lacks active signature metadata. */
  return !owner->fault&&prom_ok&&fw_ok&&stable?EFI_SUCCESS:EFI_COMPROMISED_DATA;
}
EFI_STATUS GuAd104Prepare(VOID) {
  UINT32 ss0=0,ss1=0,after0=0,after1=0;VOID *prom=NULL;struct gpu_unlock_fwsec fw;
  GuLog("GuAd104Prepare: start\n");
  /* V2 structural parsing is not signature acceptance. The dedicated
   * Turing falcon loader/selector contract is not verified in this fork. */
  if(Gu.is_turing) {
    GuLog("TURING_PREPARE=REFUSED REASON=SIGNED_LOADER_NOT_VERIFIED\n");
    return EFI_UNSUPPORTED;
  }
  if(Gu.started || Gu.fault || Gu.retained || Gu.allocations || Gu.profile_verified || Gu.prom_verified) {
    GuLog("GuAd104Prepare: state invalid (started=%u fault=%u retained=%u alloc=%p prof=%u prom=%u)\n",
          Gu.started, Gu.fault, Gu.retained, (void *)Gu.allocations, Gu.profile_verified, Gu.prom_verified);
    return EFI_NOT_READY;
  }
  if(!ConfigFor(&Gu)) {
    GuLog("GuAd104Prepare: Config() returned FALSE\n");
    return EFI_NOT_READY;
  }
  if(!RegistersFor(&Gu,&ss0,&ss1)) {
    GuLog("GuAd104Prepare: Registers() returned FALSE\n");
    return EFI_NOT_READY;
  }
  if(!GuAd104AssetsValid(&Gu)) {
    GuLog("GuAd104Prepare: Assets invalid\n");
    return EFI_CRC_ERROR;
  }
  prom=GuAlloc(1048576);
  if(!prom) {
    GuLog("GuAd104Prepare: PROM alloc failed\n");
    return EFI_OUT_OF_RESOURCES;
  }
  BOOLEAN copied=GuCopyFromIo(&Gu,prom,(VOID *)(UINTN)(Gu.bar_base+0x300000),1048576);
  BOOLEAN prom_match = copied&&!Gu.fault&&GuAd104PromMatches(&Gu,prom,1048576);
  int fwsec_res = prom_match?gpu_unlock_vbios_fwsec(prom,1048576,&fw):-1;
  BOOLEAN fwsec_match = (fwsec_res == 0) && GuAd104FwsecMatches(&fw);
  BOOLEAN cfg2 = ConfigFor(&Gu);
  BOOLEAN reg2 = cfg2&&RegistersFor(&Gu,&after0,&after1);
  BOOLEAN valid = !Gu.fault && prom_match && fwsec_match && cfg2 && reg2 && ss0==after0 && ss1==after1;
  GuLog("GuAd104Prepare: prom_match=%u fwsec_res=%d fwsec_match=%u cfg2=%u reg2=%u ss0_stable=%u\n",
        prom_match, fwsec_res, fwsec_match, cfg2, reg2, (ss0==after0 && ss1==after1));
  GuFree(prom);
  if(Gu.fault||Gu.retained)return EFI_DEVICE_ERROR;
  if(!valid) return EFI_COMPROMISED_DATA;
  Gu.prom_verified=TRUE;
  if(!GuLatchEntrySelectors(ss0,ss1)) {
    Gu.prom_verified=FALSE;
    GuLog("GuAd104Prepare: GuLatchEntrySelectors failed\n");
    return EFI_NOT_READY;
  }
  Gu.profile_verified=TRUE;
  GuLog("GuAd104Prepare: SUCCESS\n");
  return EFI_SUCCESS;
}
