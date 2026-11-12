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
  assert(GuDevicePathHasPrefix(target,sizeof(target),own_gop,sizeof(own_gop))==1);
  assert(GuDevicePathHasPrefix(target,sizeof(target),target,sizeof(target))==1);
  assert(GuDevicePathHasPrefix(target,sizeof(target),amd_gop,sizeof(amd_gop))==0);
  assert(GuDevicePathHasPrefix(target,sizeof(target),short_path,sizeof(short_path))==0);
  assert(GuDevicePathHasPrefix(target,sizeof(target),malformed,sizeof(malformed))==-1);
  assert(GuDevicePathHasPrefix(bad_parent,sizeof(bad_parent),own_gop,sizeof(own_gop))==-1);
  assert(GuDevicePathHasPrefix(target,10,own_gop,sizeof(own_gop))==-1);
  assert(GuDevicePathHasPrefix(NULL,sizeof(target),own_gop,sizeof(own_gop))==-1);
  static const unsigned char broken_suffix[] = {
    2,1,8,0,1,2,3,4,1,1,6,0,0,0,2,3,0,0
  };
  assert(GuDevicePathHasPrefix(target,sizeof(target),
                               broken_suffix,sizeof(broken_suffix))==-1);

  /* The peer NVIDIA GOP cannot substitute for an AMD display owner.
   * Unknown, overlapping, corrupt and absent PCI ownership fail closed. */
  {
    GU_DEVICE_PATH_DISPLAY_OWNER owners[3]={
      {target,sizeof(target),0x10de,0x0300},
      {amd_gop,sizeof(amd_gop),0x1002,0x0300},
      {short_path,sizeof(short_path),0x8086,0x0300}
    };
    assert(GuDevicePathAmdDisplayOwner(amd_gop,sizeof(amd_gop),owners,2)==1);
    assert(GuDevicePathAmdDisplayOwner(target,sizeof(target),owners,2)==0);
    assert(GuDevicePathAmdDisplayOwner(short_path,sizeof(short_path),owners,2)==0);
    assert(GuDevicePathAmdDisplayOwner(own_gop,sizeof(own_gop),owners,2)==0);
    assert(GuDevicePathAmdDisplayOwner(amd_gop,sizeof(amd_gop),owners,0)==-1);
    assert(GuDevicePathAmdDisplayOwner(amd_gop,sizeof(amd_gop),NULL,2)==-1);
    assert(GuDevicePathAmdDisplayOwner(NULL,sizeof(amd_gop),owners,2)==-1);
    assert(GuDevicePathAmdDisplayOwner(malformed,sizeof(malformed),owners,2)==-1);
    owners[2]=owners[1];
    assert(GuDevicePathAmdDisplayOwner(amd_gop,sizeof(amd_gop),owners,3)==-1);
    owners[2].path=malformed;owners[2].path_bytes=sizeof(malformed);
    assert(GuDevicePathAmdDisplayOwner(amd_gop,sizeof(amd_gop),owners,3)==-1);
    owners[2].path=short_path;owners[2].path_bytes=sizeof(short_path);
    owners[2].display_class=0x0604;
    assert(GuDevicePathAmdDisplayOwner(amd_gop,sizeof(amd_gop),owners,3)==-1);
    owners[2].display_class=0x0300;
    assert(GuDevicePathAmdDisplayOwner(amd_gop,sizeof(amd_gop),owners,3)==1);
    owners[1].vendor=0x10de;
    assert(GuDevicePathAmdDisplayOwner(amd_gop,sizeof(amd_gop),owners,3)==0);
    owners[1].vendor=0x1002;
    assert(GuDevicePathAmdDisplayOwner(amd_gop,4,owners,3)==-1);
  }
  puts("STATUS=COMPLETE DEVICE_PATH_AMD_OWNERSHIP_AND_BOUNDS");
  return 0;
}
