/* SPDX-License-Identifier: GPL-2.0-only */
/* Test the real production source parser, not a copy of the format rules. */
#include "../src/vendor/prep_610.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#define CHECK(x) do {if(!(x)){fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);return 1;}}while(0)
static void put32(u8 *p,u32 value) {
  p[0]=(u8)value;p[1]=(u8)(value>>8);p[2]=(u8)(value>>16);p[3]=(u8)(value>>24);
}
int main(void) {
  struct gpu_unlock_booter_hdr tu={
    .os_code_off=0,.os_code_size=256,.os_data_off=34048,.os_data_size=25088,
    .num_apps=1,.app_code_off=256,.app_code_size=33792,.app_data_off=256,
    .app_data_size=0,.patch_loc=34560,.ucode_id=3,.engine_id=1,
    .num_sigs=2,.sig_bytes=GU_BOOTER_SIG_TOTAL
  };
  CHECK(gpu_unlock_booter_validate(GU_BOOTER_TU10X,&tu,GU_BOOTER_IMAGE_SIZE_TU102,GU_BOOTER_SIG_TOTAL)==0);
  CHECK(gpu_unlock_booter_geometry_match(&tu,GU_BOOTER_IMAGE_SIZE_TU102,GU_BOOTER_SIG_TOTAL)!=NULL);
  CHECK(gpu_unlock_booter_validate(GU_BOOTER_AD10X,&tu,GU_BOOTER_IMAGE_SIZE_TU102,GU_BOOTER_SIG_TOTAL)!=0);
  CHECK(gpu_unlock_booter_validate(GU_BOOTER_GA10X,&tu,GU_BOOTER_IMAGE_SIZE_TU102,GU_BOOTER_SIG_TOTAL)!=0);
  CHECK(gpu_unlock_booter_validate(GU_BOOTER_TU10X,&tu,GU_BOOTER_IMAGE_SIZE_TU102,GU_BOOTER_SIG_TOTAL-1)!=0);
  tu.patch_loc++;CHECK(gpu_unlock_booter_geometry_match(&tu,GU_BOOTER_IMAGE_SIZE_TU102,GU_BOOTER_SIG_TOTAL)==NULL);
  tu.patch_loc--;
  u8 desc[84]={0};struct gpu_unlock_boot_desc out;
  put32(desc,4);put32(desc+4,0);put32(desc+8,512);
  put32(desc+12,1024);put32(desc+16,512);
  CHECK(gpu_unlock_boot_desc_parse(desc,76,4096,&out)==0);
  CHECK(out.image_size==4096 && out.code_offset==0);
  CHECK(gpu_unlock_boot_desc_parse(desc,3,4096,&out)!=0);
  CHECK(gpu_unlock_boot_desc_parse(desc,75,4096,&out)!=0);
  CHECK(gpu_unlock_boot_desc_parse(desc,76,24576,&out)!=0);
  CHECK(gpu_unlock_boot_desc_parse(desc,76,4096,NULL)!=0);
  put32(desc+56,1);CHECK(gpu_unlock_boot_desc_parse(desc,76,4096,&out)!=0);
  put32(desc+56,0);put32(desc+4,4095);
  CHECK(gpu_unlock_boot_desc_parse(desc,76,4096,&out)!=0);
  memset(desc,0,sizeof(desc));
  put32(desc,5);put32(desc+56,1);put32(desc+76,24576);
  for(unsigned i=0;i<5;i++) {
    const unsigned offsets[]={1,3,8,10,12};
    put32(desc+4*offsets[i],(u32)(512*(i+1)));
    put32(desc+4*(offsets[i]+1),512);
  }
  CHECK(gpu_unlock_boot_desc_parse(desc,84,24576,&out)==0);
  CHECK(out.image_size==24576);
  CHECK(gpu_unlock_boot_desc_parse(desc,76,24576,&out)!=0);
  CHECK(gpu_unlock_boot_desc_parse(desc,84,4096,&out)!=0);
  puts("PASS TU102 and v5 descriptor validation");
  return 0;
}
