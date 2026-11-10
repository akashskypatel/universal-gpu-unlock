/* SPDX-License-Identifier: GPL-2.0-only */
#include <assert.h>
#include <stdio.h>
#include "../src/gpu_device_path.h"
int main(void) {
  static const unsigned char target[] = {
    2,1,8,0,1,2,3,4, 1,1,6,0,0,0, 0x7f,0xff,4,0
  };
  static const unsigned char own_gop[] = {
    2,1,8,0,1,2,3,4, 1,1,6,0,0,0,
    2,3,8,0,0,3,1,0x80,0x7f,0xff,4,0
  };
  static const unsigned char amd_gop[] = {
    2,1,8,0,1,2,3,4, 1,1,6,0,4,0,
    2,3,8,0,0,3,1,0x80,0x7f,0xff,4,0
  };
  static const unsigned char short_path[] = {
    2,1,8,0,1,2,3,4,0x7f,0xff,4,0
  };
  static const unsigned char malformed[] = {
    2,1,8,0,1,2,3,4,1,1,0,0,0,0,0,0,0,0
  };
  static const unsigned char bad_parent[] = {
    2,1,8,0,1,2,3,4,1,1,6,0,0,0,0x7f,0,4,0
  };
  assert(GuDevicePathHasPrefix(target,own_gop,sizeof(own_gop))==1);
  assert(GuDevicePathHasPrefix(target,target,sizeof(target))==1);
  assert(GuDevicePathHasPrefix(target,amd_gop,sizeof(amd_gop))==0);
  assert(GuDevicePathHasPrefix(target,short_path,sizeof(short_path))==0);
  assert(GuDevicePathHasPrefix(target,malformed,sizeof(malformed))==-1);
  assert(GuDevicePathHasPrefix(bad_parent,own_gop,sizeof(own_gop))==-1);
  assert(GuDevicePathHasPrefix(target,own_gop,10)==-1);
  assert(GuDevicePathHasPrefix(NULL,own_gop,sizeof(own_gop))==-1);
  puts("STATUS=COMPLETE PASS=8 DEVICE_PATH_BOUNDS");
  return 0;
}
