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
    assert(GuDevicePathAmdDisplayOwner(amd_gop,sizeof(amd_gop),owners,3)==-1);
    owners[2].path=own_gop;owners[2].path_bytes=sizeof(own_gop);
    assert(GuDevicePathAmdDisplayOwner(amd_gop,sizeof(amd_gop),owners,3)==1);
    owners[1].vendor=0x10de;
    assert(GuDevicePathAmdDisplayOwner(amd_gop,sizeof(amd_gop),owners,3)==0);
    owners[1].vendor=0x1002;
    assert(GuDevicePathAmdDisplayOwner(amd_gop,4,owners,3)==-1);
  }
  /* Original B850 read-only diagnostic 0018: AMD 0000:0f:00.0
   * controller and GOP; 0021: NVIDIA 0000:04:00.0 peer and GOP.
   * These saved paths are static fixtures, not a new physical test. */
  {
    static const unsigned char amd_pci[]={
      0x02,0x01,0x0c,0x00,0xd0,0x41,0x03,0x0a,0,0,0,0,
      0x01,0x01,0x06,0,0x01,0x08,0x01,0x01,0x06,0,0,0,
      0x7f,0xff,0x04,0
    };
    static const unsigned char amd_console[]={
      0x02,0x01,0x0c,0,0xd0,0x41,0x03,0x0a,0,0,0,0,
      0x01,0x01,0x06,0,0x01,0x08,0x01,0x01,0x06,0,0,0,
      0x02,0x03,0x08,0,0x00,0x03,0x01,0x80,0x7f,0xff,0x04,0
    };
    static const unsigned char nvidia_pci[]={
      0x02,0x01,0x0c,0,0xd0,0x41,0x03,0x0a,0,0,0,0,
      0x01,0x01,0x06,0,0x01,0x02,0x01,0x01,0x06,0,0,0,
      0x01,0x01,0x06,0,0,0,0x01,0x01,0x06,0,0,0,
      0x7f,0xff,0x04,0
    };
    static const unsigned char nvidia_console[]={
      0x02,0x01,0x0c,0,0xd0,0x41,0x03,0x0a,0,0,0,0,
      0x01,0x01,0x06,0,0x01,0x02,0x01,0x01,0x06,0,0,0,
      0x01,0x01,0x06,0,0,0,0x01,0x01,0x06,0,0,0,
      0x02,0x03,0x08,0,0x40,0x03,0x01,0x80,0x7f,0xff,0x04,0
    };
    GU_DEVICE_PATH_DISPLAY_OWNER original[2]={
      {nvidia_pci,sizeof(nvidia_pci),0x10de,0x0300},
      {amd_pci,sizeof(amd_pci),0x1002,0x0300}
    };
    assert(sizeof(amd_pci)==28);
    assert(sizeof(amd_console)==36);
    assert(sizeof(nvidia_pci)==40);
    assert(sizeof(nvidia_console)==48);
    assert(GuDevicePathAmdDisplayOwner(
        amd_console,sizeof(amd_console),original,2)==1);
    assert(GuDevicePathAmdDisplayOwner(
        nvidia_console,sizeof(nvidia_console),original,2)==0);
    /* A peer-only GOP cannot replace the independent AMD output. */
    assert(GuDevicePathAmdDisplayOwner(
        nvidia_console,sizeof(nvidia_console),original,1)==0);
    /* An unplugged/replaced AMD identity must not pass unchanged paths. */
    original[1].vendor=0x10de;
    assert(GuDevicePathAmdDisplayOwner(
        amd_console,sizeof(amd_console),original,2)==0);
    original[1].vendor=0x1002;
    original[1].display_class=0x0604;
    assert(GuDevicePathAmdDisplayOwner(
        amd_console,sizeof(amd_console),original,2)==-1);
  }
  /* A mismatched first node must not hide a corrupt later node.
   * The old prefix comparator returned 0 before inspecting that suffix. */
  {
    static const unsigned char corrupt_unrelated[]={
      3,1,8,0,1,2,3,4,1,1,0,0,0,0
    };
    assert(GuDevicePathValid(target,sizeof(target))==1);
    assert(GuDevicePathValid(corrupt_unrelated,sizeof(corrupt_unrelated))==0);
    assert(GuDevicePathHasPrefix(target,sizeof(target),
                                 corrupt_unrelated,sizeof(corrupt_unrelated))==-1);
    assert(GuDevicePathHasPrefix(corrupt_unrelated,sizeof(corrupt_unrelated),
                                 target,sizeof(target))==-1);
    GU_DEVICE_PATH_DISPLAY_OWNER unrelated[2]={
      {amd_gop,sizeof(amd_gop),0x1002,0x0300},
      {corrupt_unrelated,sizeof(corrupt_unrelated),0x10de,0x0300}
    };
    assert(GuDevicePathAmdDisplayOwner(
        amd_gop,sizeof(amd_gop),unrelated,2)==-1);
    assert(GuDevicePathAmdDisplayOwner(
        corrupt_unrelated,sizeof(corrupt_unrelated),unrelated,1)==-1);
    /* An invalid END_INSTANCE is not a complete single-instance GOP. */
    static const unsigned char bad_end[]={
      3,1,8,0,1,2,3,4,0x7f,0x01,4,0
    };
    assert(GuDevicePathHasPrefix(target,sizeof(target),bad_end,sizeof(bad_end))==-1);
  }
  /* Surviving GOP paths must remain identical across vendor Stop and
   * Windows handover. The owner may not be silently retargeted. */
  {
    static const unsigned char original[]={
      2,1,8,0,1,2,3,4,1,1,6,0,4,0,
      2,3,8,0,0,3,1,0x80,0x7f,0xff,4,0
    };
    unsigned char changed[sizeof(original)];
    memcpy(changed,original,sizeof(original));
    assert(GuDevicePathLength(original,sizeof(original))==sizeof(original));
    assert(GuDevicePathUnchanged(original,sizeof(original),
            changed,sizeof(changed))==1);
    changed[16]=9; /* Alter the ADR payload while preserving path length. */
    assert(GuDevicePathValid(changed,sizeof(changed))==1);
    assert(GuDevicePathUnchanged(original,sizeof(original),
            changed,sizeof(changed))==0);
    changed[16]=8;
    changed[sizeof(changed)-3]=1; /* END_INSTANCE, not END_ENTIRE. */
    assert(GuDevicePathUnchanged(original,sizeof(original),
            changed,sizeof(changed))==0);
    assert(GuDevicePathUnchanged(original,sizeof(original)-1,
            original,sizeof(original))==0);
    assert(GuDevicePathUnchanged(original,sizeof(original),
            original,sizeof(original)-1)==0);
    assert(GuDevicePathLength(NULL,sizeof(original))==0);
  }
  puts("STATUS=COMPLETE DEVICE_PATH_AMD_OWNERSHIP_AND_BOUNDS");
  return 0;
}
