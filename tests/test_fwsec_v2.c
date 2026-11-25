/* SPDX-License-Identifier: GPL-2.0-only */
/* Host execution of the original bounded firmware descriptor parser.
 * Test V2 structural admission and V3 signature metadata separately. */
#include "../src/vendor/vbios_610.c"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if(!(x)) { fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);return 1; } }while(0)
static void wr16(u8 *p,u16 x) {p[0]=(u8)x;p[1]=(u8)(x>>8);}
static void wr32(u8 *p,u32 x) {wr16(p,(u16)x);wr16(p+2,(u16)(x>>16));}
static void make_v2(u8 *p) {
  memset(p,0,2048);
  wr32(p,(60U<<16)|(2U<<8)|1U);
  wr32(p+4,256);  /* Stored uncompressed microcode bytes. */
  wr32(p+8,256);  /* Uncompressed bytes. */
  wr32(p+16,8);   /* Interface offset within DMEM. */
  wr32(p+20,0);   /* IMEM physical base. */
  wr32(p+24,128); /* Entire IMEM code length. */
  wr32(p+28,0x1000); /* IMEM virtual base. */
  wr32(p+32,0x1020); /* Secure region virtual address. */
  wr32(p+36,32); /* Secure region length. */
  wr32(p+40,128); /* DMEM image offset. */
  wr32(p+44,0);  /* DMEM physical base. */
  wr32(p+48,64); /* DMEM data length. */
}
static void make_v3(u8 *p) {
  memset(p,0,2048);
  wr32(p,(428U<<16)|(3U<<8)|1U);
  wr32(p+4,768); /* IMEM + sufficiently large DMEM image. */
  wr32(p+8,0); /* PKC destination in DMEM. */
  wr32(p+12,8); /* Interface offset. */
  wr32(p+16,0); /* IMEM physical. */
  wr32(p+20,256); /* IMEM size. */
  wr32(p+24,0); /* IMEM virtual. */
  wr32(p+28,0); /* DMEM physical. */
  wr32(p+32,512); /* DMEM size (contains 384-byte signature). */
  wr16(p+36,0x400);p[38]=9;p[39]=1;wr16(p+40,1);
}
/* A captured PROM window, parsed by the same walk the transaction uses.
 *
 * A capture from the bench is exactly 1 MiB, and nothing else in this repository
 * can say whether it carries a descriptor the transaction would accept. The
 * four admission bits printed here are the conditions of
 * GuAd104FwsecMatches() in src/profile.c - tests/test_vram_block_probe.py
 * checks that those are still the four, so this report cannot drift from the
 * decision the firmware makes. SIGNATURE_AUTHENTICATED stays 0: this is a
 * structural walk over bytes, not a signature check.
 */
static int parse_file(const char *path) {
  FILE *f;
  long size;
  size_t read_bytes;
  u8 *rom;
  struct gpu_unlock_fwsec fw;
  int ret, v3, ucode, count, sigbytes, accepted;
  f=fopen(path,"rb");
  if(!f){fprintf(stderr,"cannot open %s\n",path);return 2;}
  if(fseek(f,0,SEEK_END)!=0){fclose(f);return 2;}
  size=ftell(f);
  if(size<=0 || size>1024*1024){fclose(f);fprintf(stderr,"unusable size %ld\n",size);return 2;}
  rewind(f);
  rom=(u8 *)malloc((size_t)size);
  if(!rom){fclose(f);return 2;}
  read_bytes=fread(rom,1,(size_t)size,f);
  fclose(f);
  if(read_bytes!=(size_t)size){free(rom);fprintf(stderr,"short read\n");return 2;}
  printf("PROM_FILE PATH=%s BYTES=%ld\n",path,size);
  ret=gpu_unlock_vbios_fwsec(rom,(size_t)size,&fw);
  free(rom);
  if(ret){
    printf("PROM_FILE FWSEC_FOUND=0 PARSER_RESULT=%d\n",ret);
    return 1;
  }
  v3=fw.descriptor_version==3;
  ucode=fw.ucode_id!=0;
  count=fw.sig_count>=1;
  sigbytes=fw.sig_bytes==(u32)fw.sig_count*384;
  accepted=v3&&ucode&&count&&sigbytes;
  printf("PROM_FILE FWSEC_FOUND=1 DESCRIPTOR_VERSION=%u IMAGE_OFF=%u IMAGE_SIZE=%u "
         "IMEM_SIZE=%u DMEM_SIZE=%u ENGINE_MASK=%#x UCODE_ID=%u SIG_COUNT=%u "
         "SIG_VERSIONS=%#x SIG_BYTES=%u\n",
         fw.descriptor_version,fw.image_off,fw.image_size,fw.imem_size,
         fw.dmem_size,fw.engine_mask,fw.ucode_id,fw.sig_count,fw.sig_versions,
         fw.sig_bytes);
  printf("PROM_FILE ADMISSION DESCRIPTOR_V3=%d UCODE_NONZERO=%d "
         "SIG_COUNT_AT_LEAST_1=%d SIG_BYTES_384_PER_COUNT=%d ACCEPTED=%d "
         "SIGNATURE_AUTHENTICATED=0\n",
         v3,ucode,count,sigbytes,accepted);
  return accepted?0:1;
}

int main(int argc, char **argv) {
  if(argc>1)return parse_file(argv[1]);
  u8 rom[2048]={0};
  struct gpu_unlock_fwsec fw;
  make_v2(rom);
  CHECK(descriptor(rom,sizeof(rom),0,&fw)==0);
  CHECK(fw.descriptor_version==2 && fw.image_off==60 && fw.image_size==256);
  CHECK(fw.imem_size==128 && fw.data_off==128 && fw.dmem_size==64);
  CHECK(!fw.sig_bytes && !fw.sig_count && !fw.sig_off && !fw.ucode_id &&
        !fw.engine_mask && !fw.sig_versions && !fw.pkc_off);
  CHECK(descriptor(rom,59,0,&fw)!=0);
  wr32(rom,(59U<<16)|(2U<<8)|1U);
  CHECK(descriptor(rom,sizeof(rom),0,&fw)!=0); /* V2 header under 60. */
  make_v2(rom); wr32(rom+4,128);CHECK(descriptor(rom,sizeof(rom),0,&fw)!=0);
  make_v2(rom); wr32(rom+8,0);CHECK(descriptor(rom,sizeof(rom),0,&fw)!=0);
  make_v2(rom); wr32(rom+40,240);CHECK(descriptor(rom,sizeof(rom),0,&fw)!=0);
  make_v2(rom); wr32(rom+40,100);CHECK(descriptor(rom,sizeof(rom),0,&fw)!=0);
  make_v2(rom); wr32(rom+32,0xfff);CHECK(descriptor(rom,sizeof(rom),0,&fw)!=0);
  make_v2(rom); wr32(rom+32,0x1080);CHECK(descriptor(rom,sizeof(rom),0,&fw)!=0);
  make_v2(rom); wr32(rom+36,120);CHECK(descriptor(rom,sizeof(rom),0,&fw)!=0);
  make_v2(rom); wr32(rom+52,1);CHECK(descriptor(rom,sizeof(rom),0,&fw)!=0);
  make_v2(rom); wr32(rom+56,1);CHECK(descriptor(rom,sizeof(rom),0,&fw)!=0);
  make_v2(rom); wr32(rom+16,64);CHECK(descriptor(rom,sizeof(rom),0,&fw)!=0);
  make_v2(rom); wr32(rom+4,0xffffffffU);CHECK(descriptor(rom,sizeof(rom),0,&fw)!=0);
  make_v2(rom);CHECK(descriptor(rom,315,0,&fw)!=0); /* Image ends at 316. */
  make_v2(rom);wr32(rom,(60U<<16)|(4U<<8)|1U);
  CHECK(descriptor(rom,sizeof(rom),0,&fw)!=0); /* Unknown version. */
  make_v3(rom);
  CHECK(descriptor(rom,sizeof(rom),0,&fw)==0);
  CHECK(fw.descriptor_version==3 && fw.sig_count==1 &&
        fw.sig_bytes==384 && fw.sig_off==44 && fw.ucode_id==9);
  make_v3(rom);wr32(rom,(43U<<16)|(3U<<8)|1U);
  CHECK(descriptor(rom,sizeof(rom),0,&fw)!=0);
  make_v3(rom);rom[39]=2;CHECK(descriptor(rom,sizeof(rom),0,&fw)!=0);
  puts("PASS FWSEC V2 structural-only and V3 signature metadata");
  return 0;
}
