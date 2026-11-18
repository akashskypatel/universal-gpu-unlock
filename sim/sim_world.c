/* SPDX-License-Identifier: GPL-2.0-only */
/* Host simulator: UEFI services, handles, console and directory-backed file
 * systems. This is a model of the firmware environment, not firmware. */
#define _GNU_SOURCE 1
#include "sim_world.h"
#include "boot_profile.h"

#include <Guid/FileInfo.h>
#include <Protocol/DriverBinding.h>
#include <Protocol/GraphicsOutput.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <dirent.h>
#include <sys/types.h>
#include <unistd.h>

#define SIM_LOG_BYTES (4u << 20)

static SIM_HANDLE   g_handles[SIM_MAX_HANDLES];
static unsigned     g_handle_count;
static SIM_VOLUME   g_volumes[SIM_MAX_VOLUMES];
static unsigned     g_volume_count;
static SIM_STATS    g_stats;
static SIM_GPU     *g_gpu;
static EFI_HANDLE   g_image_handle;

static EFI_BOOT_SERVICES    g_bs;
static EFI_RUNTIME_SERVICES g_rt;
static EFI_SYSTEM_TABLE     g_st;
static EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL g_conout;
static EFI_SIMPLE_TEXT_INPUT_PROTOCOL  g_conin;
static EFI_HANDLE                      g_console_handle;
static EFI_HANDLE                      g_driver_handle;
static UINT8                          *g_amd_path;
static UINTN                           g_amd_path_length;

static char   g_log[SIM_LOG_BYTES];
static size_t g_log_used;
static int    g_failures;

static CHAR16 g_key_queue[8];
static unsigned g_key_count, g_key_at;

/* ------------------------------------------------------------------ */
/* Handles                                                             */
/* ------------------------------------------------------------------ */

static SIM_HANDLE *HandleNew(const char *name) {
  if (g_handle_count >= SIM_MAX_HANDLES) return NULL;
  SIM_HANDLE *h = &g_handles[g_handle_count++];
  memset(h, 0, sizeof(*h));
  h->handle = (EFI_HANDLE)h;
  h->name = name;
  return h;
}

static SIM_HANDLE *HandleOf(EFI_HANDLE handle) {
  for (unsigned i = 0; i < g_handle_count; i++)
    if (g_handles[i].handle == handle) return &g_handles[i];
  return NULL;
}

static void HandleAdd(SIM_HANDLE *h, const EFI_GUID *guid, void *iface) {
  if (!h || !guid || !iface || h->count >= SIM_MAX_PROTOCOLS) return;
  h->protocols[h->count].guid = *guid;
  h->protocols[h->count].iface = iface;
  h->count++;
}

static void *HandleFind(SIM_HANDLE *h, const EFI_GUID *guid) {
  if (!h || !guid) return NULL;
  for (unsigned i = 0; i < h->count; i++)
    if (!memcmp(&h->protocols[i].guid, guid, sizeof(*guid)))
      return h->protocols[i].iface;
  return NULL;
}

/* ------------------------------------------------------------------ */
/* Pool and DMA-capable buffers                                        */
/* ------------------------------------------------------------------ */

static EFI_STATUS EFIAPI SimAllocatePool(EFI_MEMORY_TYPE type, UINTN bytes,
                                         VOID **out) {
  (void)type;
  if (!out) return EFI_INVALID_PARAMETER;
  *out = NULL;
  if (!bytes) return EFI_INVALID_PARAMETER;
  void *p = calloc(1, bytes ? bytes : 1);
  if (!p) { g_stats.pool_failures++; return EFI_OUT_OF_RESOURCES; }
  g_stats.allocations++;
  *out = p;
  return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI SimFreePool(VOID *p) {
  if (!p) return EFI_INVALID_PARAMETER;
  g_stats.frees++;
  free(p);
  return EFI_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* Log capture                                                         */
/* ------------------------------------------------------------------ */

static void LogAppend(const char *bytes, size_t n) {
  if (!bytes || !n) return;
  size_t room = sizeof(g_log) - g_log_used;
  if (n > room) n = room;
  if (!n) return;
  memcpy(g_log + g_log_used, bytes, n);
  g_log_used += n;
  g_log[g_log_used < sizeof(g_log) ? g_log_used : sizeof(g_log) - 1] = 0;
}

void SimSay(const char *text) {
  if (text) LogAppend(text, strlen(text));
}

const char *SimLogText(void) { return g_log; }
size_t SimLogLength(void) { return g_log_used; }

int SimLogContains(const char *needle) {
  return needle && strstr(g_log, needle) != NULL;
}

UINTN SimLogCount(const char *needle) {
  UINTN count = 0;
  if (!needle || !*needle) return 0;
  for (const char *at = g_log; (at = strstr(at, needle)) != NULL; at += strlen(needle))
    count++;
  return count;
}

void SimConsoleKey(CHAR16 key) {
  if (g_key_count < sizeof(g_key_queue) / sizeof(g_key_queue[0]))
    g_key_queue[g_key_count++] = key;
}

/* ------------------------------------------------------------------ */
/* Console                                                             */
/* ------------------------------------------------------------------ */

static EFI_STATUS EFIAPI SimOutReset(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *self,
                                     BOOLEAN extended) {
  (void)self;
  (void)extended;
  return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI SimOutString(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *self,
                                      CHAR16 *text) {
  (void)self;
  char buffer[4096];
  size_t used = 0;
  if (!text) return EFI_INVALID_PARAMETER;
  for (UINTN i = 0; text[i] && used + 2 < sizeof(buffer); i++)
    buffer[used++] = (text[i] < 0x80) ? (char)text[i] : '?';
  buffer[used] = 0;
  LogAppend(buffer, used);
  return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI SimInReset(EFI_SIMPLE_TEXT_INPUT_PROTOCOL *self,
                                    BOOLEAN extended) {
  (void)self;
  (void)extended;
  g_key_at = 0;
  return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI SimInReadKey(EFI_SIMPLE_TEXT_INPUT_PROTOCOL *self,
                                      EFI_INPUT_KEY *key) {
  (void)self;
  if (!key) return EFI_INVALID_PARAMETER;
  key->ScanCode = 0;
  key->UnicodeChar = 0;
  if (g_key_at >= g_key_count) return EFI_NOT_READY;
  key->UnicodeChar = g_key_queue[g_key_at++];
  return EFI_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* File system                                                         */
/* ------------------------------------------------------------------ */

#define SIM_MAX_OPEN_FILES 32

typedef struct {
  EFI_FILE_PROTOCOL proto;
  FILE             *fp;
  int               used;
  int               is_dir;
  int               writable;
  int               volume;
  char              path[1024];
  UINT64            size;
} SIM_FILE;

static SIM_FILE g_files[SIM_MAX_OPEN_FILES];

static void WidenToAscii(const CHAR16 *in, char *out, size_t out_bytes) {
  size_t used = 0;
  if (!in || !out || !out_bytes) return;
  for (UINTN i = 0; in[i] && used + 1 < out_bytes; i++)
    used += (size_t)snprintf(out + used, out_bytes - used, "%c",
                             (in[i] < 0x80) ? (char)in[i] : '?');
  out[used < out_bytes ? used : out_bytes - 1] = 0;
}

static void NormalizeSlashes(char *path) {
  for (char *p = path; *p; p++)
    if (*p == '\\') *p = '/';
}

static int HostFileSize(const char *path, UINT64 *size, int *is_dir) {
  struct stat st;
  if (stat(path, &st) != 0) return 0;
  if (S_ISDIR(st.st_mode)) { if (is_dir) *is_dir = 1; if (size) *size = 0; return 1; }
  if (is_dir) *is_dir = 0;
  if (size) *size = (UINT64)st.st_size;
  return 1;
}

static int HostExists(const char *path) {
  struct stat st;
  return stat(path, &st) == 0;
}

/* FAT names are case-insensitive while the host filesystem is not, so a
 * component that does not exist verbatim gets one case-insensitive scan. */
static int ScanDirForName(const char *dir, const char *name, char *out,
                          size_t out_bytes) {
  DIR *handle = opendir(dir && dir[0] ? dir : ".");
  if (!handle) return 0;
  int found = 0;
  struct dirent *entry;
  while ((entry = readdir(handle)) != NULL) {
    if (strcasecmp(entry->d_name, name) != 0) continue;
    snprintf(out, out_bytes, "%s%s%s", dir && dir[0] ? dir : "",
             dir && dir[0] && dir[strlen(dir) - 1] != '/' ? "/" : "",
             entry->d_name);
    found = 1;
    break;
  }
  closedir(handle);
  return found;
}

static int ResolveCaseInsensitive(const char *path, char *out, size_t out_bytes) {
  char work[1024];
  snprintf(work, sizeof(work), "%s", path);
  size_t used = 0;
  out[0] = 0;
  if (work[0] == '/') { out[0] = '/'; out[1] = 0; used = 1; }
  char *cursor = work;
  while (*cursor) {
    while (*cursor == '/') cursor++;
    if (!*cursor) break;
    char *slash = strchr(cursor, '/');
    if (slash) *slash = 0;
    char candidate[1200];
    if (used == 0)
      snprintf(candidate, sizeof(candidate), "%s", cursor);
    else
      snprintf(candidate, sizeof(candidate), "%s%s%s", out,
               out[used - 1] == '/' ? "" : "/", cursor);
    if (HostExists(candidate)) {
      snprintf(out, out_bytes, "%s", candidate);
    } else {
      char parent[1024];
      snprintf(parent, sizeof(parent), "%s", out);
      if (!ScanDirForName(parent[0] ? parent : ".", cursor, out, out_bytes))
        return 0;
    }
    used = strlen(out);
    if (!slash) break;
    cursor = slash + 1;
  }
  return used > 0;
}

static int MakeDirectory(const char *path) {
  if (mkdir(path, 0777) == 0) return 0;
  struct stat st;
  if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) return 0;
  return -1;
}

static int SimDebug(void) {
  static int cached = -1;
  if (cached < 0) cached = getenv("SIM_DEBUG") != NULL;
  return cached;
}

static EFI_STATUS EFIAPI SimFileOpen(EFI_FILE_PROTOCOL *self, EFI_FILE_PROTOCOL **out,
                              CHAR16 *name, UINT64 mode, UINT64 attributes);
static EFI_STATUS EFIAPI SimFileClose(EFI_FILE_PROTOCOL *self);
static EFI_STATUS EFIAPI SimFileRead(EFI_FILE_PROTOCOL *self, UINTN *size, VOID *buffer);
static EFI_STATUS EFIAPI SimFileWrite(EFI_FILE_PROTOCOL *self, UINTN *size, VOID *buffer);
static EFI_STATUS EFIAPI SimFileGetInfo(EFI_FILE_PROTOCOL *self, EFI_GUID *info_type,
                                 UINTN *size, VOID *buffer);
static EFI_STATUS EFIAPI SimFileFlush(EFI_FILE_PROTOCOL *self);

static void FileInit(SIM_FILE *f, int volume, const char *path, int is_dir,
                     int writable) {
  memset(f, 0, sizeof(*f));
  f->used = 1;
  f->is_dir = is_dir;
  f->writable = writable;
  f->volume = volume;
  if (SimDebug()) fprintf(stderr, "SIM_FS_INIT f=%p volume=%d is_dir=%d path='%s'\n",
                          (void *)f, volume, is_dir, path ? path : "");
  snprintf(f->path, sizeof(f->path), "%s", path ? path : "");
  f->proto.Revision = EFI_FILE_PROTOCOL_REVISION;
  f->proto.Open = SimFileOpen;
  f->proto.Close = SimFileClose;
  f->proto.Read = SimFileRead;
  f->proto.Write = SimFileWrite;
  f->proto.GetInfo = SimFileGetInfo;
  f->proto.Flush = SimFileFlush;
}

static SIM_FILE *FileSlot(void) {
  for (unsigned i = 0; i < SIM_MAX_OPEN_FILES; i++)
    if (!g_files[i].used) return &g_files[i];
  return NULL;
}

/* Resolve a name against a volume root or an open directory handle. Absolute
 * names (leading backslash) are addressed from the volume root even when the
 * caller holds a subdirectory handle. */
static int ResolvePath(SIM_FILE *self, const CHAR16 *name, char *out,
                       size_t out_bytes) {
  char ascii[768];
  if (!self || self->volume < 0 || (unsigned)self->volume >= g_volume_count) {
    if (SimDebug()) fprintf(stderr, "SIM_FS_RESOLVE_FAIL volume=%d count=%u\n",
                            self ? self->volume : -99, g_volume_count);
    return -1;
  }
  WidenToAscii(name, ascii, sizeof(ascii));
  NormalizeSlashes(ascii);
  int absolute = (ascii[0] == '/');
  const char *relative = ascii;
  while (*relative == '/') relative++;
  if (!relative[0]) return -1;

  const char *base = absolute || !self->path[0]
                         ? g_volumes[self->volume].host_root
                         : self->path;
  size_t base_len = strlen(base);
  char trimmed[900];
  snprintf(trimmed, sizeof(trimmed), "%s", base);
  if (base_len && trimmed[base_len - 1] == '/') trimmed[base_len - 1] = 0;
  snprintf(out, out_bytes, "%s/%s", trimmed, relative);
  if (!HostExists(out)) {
    char fixed[1024];
    if (ResolveCaseInsensitive(out, fixed, sizeof(fixed)))
      snprintf(out, out_bytes, "%s", fixed);
  }
  return 0;
}

static EFI_STATUS EFIAPI SimFileOpen(EFI_FILE_PROTOCOL *self, EFI_FILE_PROTOCOL **out,
                              CHAR16 *name, UINT64 mode, UINT64 attributes) {
  if (!self || !out || !name) return EFI_INVALID_PARAMETER;
  *out = NULL;
  SIM_FILE *file = (SIM_FILE *)self;
  char host[1024];
  if (SimDebug()) fprintf(stderr, "SIM_FS_OPEN_ENTER self=%p slot=%ld volume=%d\n",
                          (void *)self, (long)((SIM_FILE *)self - g_files),
                          ((SIM_FILE *)self)->volume);
  if (ResolvePath(file, name, host, sizeof(host)) != 0)
    return EFI_INVALID_PARAMETER;
  if (SimDebug()) {
    char shown[768];
    WidenToAscii(name, shown, sizeof(shown));
    fprintf(stderr, "SIM_FS_OPEN name='%s' host='%s' mode=%llx attr=%llx\n",
            shown, host, (unsigned long long)mode, (unsigned long long)attributes);
  }

  UINT64 size = 0;
  int is_dir = 0;
  int exists = HostFileSize(host, &size, &is_dir);

  if (attributes & EFI_FILE_DIRECTORY) {
    if (!exists && (mode & EFI_FILE_MODE_CREATE)) {
      if (MakeDirectory(host) != 0) return EFI_DEVICE_ERROR;
      exists = 1;
      is_dir = 1;
    }
    if (!exists || !is_dir) return EFI_NOT_FOUND;
    SIM_FILE *slot = FileSlot();
    if (!slot) return EFI_OUT_OF_RESOURCES;
    FileInit(slot, file->volume, host, 1, 1);
    *out = &slot->proto;
    return EFI_SUCCESS;
  }

  if (exists && is_dir) return EFI_NOT_FOUND;

  const char *open_mode;
  if (mode & EFI_FILE_MODE_WRITE) {
    if (!exists && !(mode & EFI_FILE_MODE_CREATE)) return EFI_NOT_FOUND;
    if (!exists) {
      char parent[1024];
      snprintf(parent, sizeof(parent), "%s", host);
      char *slash = strrchr(parent, '/');
      if (slash) { *slash = 0; MakeDirectory(parent); }
    }
    open_mode = exists ? "ab" : "wb";
  } else {
    if (!exists) return EFI_NOT_FOUND;
    open_mode = "rb";
  }

  SIM_FILE *slot = FileSlot();
  if (!slot) return EFI_OUT_OF_RESOURCES;
  FILE *fp = fopen(host, open_mode);
  if (!fp) return exists ? EFI_ACCESS_DENIED : EFI_NOT_FOUND;
  FileInit(slot, file->volume, host, 0, (mode & EFI_FILE_MODE_WRITE) != 0);
  slot->fp = fp;
  if (fseek(fp, 0, SEEK_END) == 0) slot->size = (UINT64)ftell(fp);
  fseek(fp, 0, SEEK_SET);
  *out = &slot->proto;
  return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI SimFileClose(EFI_FILE_PROTOCOL *self) {
  if (!self) return EFI_INVALID_PARAMETER;
  SIM_FILE *file = (SIM_FILE *)self;
  if (file->fp) { fclose(file->fp); file->fp = NULL; }
  file->used = 0;
  return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI SimFileRead(EFI_FILE_PROTOCOL *self, UINTN *size,
                              VOID *buffer) {
  if (!self || !size || !buffer) return EFI_INVALID_PARAMETER;
  SIM_FILE *file = (SIM_FILE *)self;
  if (!file->fp || file->is_dir) return EFI_UNSUPPORTED;
  size_t n = fread(buffer, 1, *size, file->fp);
  *size = n;
  return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI SimFileWrite(EFI_FILE_PROTOCOL *self, UINTN *size,
                               VOID *buffer) {
  if (!self || !size || !buffer) return EFI_INVALID_PARAMETER;
  SIM_FILE *file = (SIM_FILE *)self;
  if (!file->fp || file->is_dir) return EFI_UNSUPPORTED;
  if (!file->writable) return EFI_ACCESS_DENIED;
  size_t n = fwrite(buffer, 1, *size, file->fp);
  *size = n;
  /* Mirror every durable line into the run log so the harness can assert on
   * exactly what the image wrote. */
  LogAppend((const char *)buffer, n);
  return (n == *size) ? EFI_SUCCESS : EFI_DEVICE_ERROR;
}

static EFI_STATUS EFIAPI SimFileFlush(EFI_FILE_PROTOCOL *self) {
  if (!self) return EFI_INVALID_PARAMETER;
  SIM_FILE *file = (SIM_FILE *)self;
  if (!file->fp) return EFI_UNSUPPORTED;
  return fflush(file->fp) == 0 ? EFI_SUCCESS : EFI_DEVICE_ERROR;
}

static EFI_STATUS EFIAPI SimFileGetInfo(EFI_FILE_PROTOCOL *self, EFI_GUID *info_type,
                                 UINTN *size, VOID *buffer) {
  static const EFI_GUID file_info_guid = EFI_FILE_INFO_ID;
  if (!self || !info_type || !size || !buffer) return EFI_INVALID_PARAMETER;
  if (memcmp(info_type, &file_info_guid, sizeof(EFI_GUID)) != 0)
    return EFI_UNSUPPORTED;
  SIM_FILE *file = (SIM_FILE *)self;
  UINT64 file_size = 0;
  int is_dir = file->is_dir;
  if (!is_dir) {
    UINT64 measured = 0;
    if (!HostFileSize(file->path, &measured, &is_dir)) return EFI_DEVICE_ERROR;
    if (file->fp) fflush(file->fp);
    file_size = measured;
  }
  const char *base = strrchr(file->path, '/');
  base = base ? base + 1 : file->path;
  UINTN name_chars = strlen(base);
  UINTN needed = sizeof(EFI_FILE_INFO) + (name_chars + 1) * sizeof(CHAR16);
  if (*size < needed) { *size = needed; return EFI_BUFFER_TOO_SMALL; }
  memset(buffer, 0, *size);
  EFI_FILE_INFO *info = (EFI_FILE_INFO *)buffer;
  info->Size = needed;
  info->FileSize = file_size;
  info->PhysicalSize = file_size;
  info->Attribute = is_dir ? EFI_FILE_DIRECTORY : 0;
  for (UINTN i = 0; i < name_chars; i++) info->FileName[i] = (CHAR16)base[i];
  info->FileName[name_chars] = 0;
  *size = needed;
  return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI SimVolumeOpen(EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *self,
                                       EFI_FILE_PROTOCOL **root) {
  if (!self || !root) return EFI_INVALID_PARAMETER;
  *root = NULL;
  int index = -1;
  SIM_FILE *slot = FileSlot();
  for (unsigned i = 0; i < g_volume_count; i++)
    if (&g_volumes[i].fs == self) { index = (int)i; break; }
  if (SimDebug()) fprintf(stderr, "SIM_FS_OPENVOLUME index=%d\n", index);
  if (index < 0) return EFI_INVALID_PARAMETER;
  if (!slot) return EFI_OUT_OF_RESOURCES;
  FileInit(slot, index, "", 1, 1);
  *root = &slot->proto;
  if (SimDebug()) fprintf(stderr, "SIM_FS_ROOT=%p\n", (void *)*root);
  return EFI_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* Boot services                                                       */
/* ------------------------------------------------------------------ */

static EFI_STATUS EFIAPI SimHandleProtocol(EFI_HANDLE handle, EFI_GUID *guid,
                                           VOID **iface) {
  g_stats.handle_protocol_calls++;
  if (!guid || !iface) return EFI_INVALID_PARAMETER;
  *iface = NULL;
  SIM_HANDLE *h = HandleOf(handle);
  void *found = HandleFind(h, guid);
  if (!found) return EFI_UNSUPPORTED;
  *iface = found;
  return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI SimLocateHandleBuffer(EFI_LOCATE_SEARCH_TYPE type,
                                               EFI_GUID *guid, VOID *key,
                                               UINTN *count,
                                               EFI_HANDLE **buffer) {
  (void)key;
  g_stats.locate_calls++;
  if (!count || !buffer) return EFI_INVALID_PARAMETER;
  *count = 0;
  *buffer = NULL;
  if (type != ByProtocol && type != AllHandles) return EFI_UNSUPPORTED;
  if (type == ByProtocol && !guid) return EFI_INVALID_PARAMETER;
  if (type == AllHandles) {
    if (!g_handle_count) return EFI_NOT_FOUND;
    EFI_HANDLE *all = calloc(g_handle_count, sizeof(EFI_HANDLE));
    if (!all) return EFI_OUT_OF_RESOURCES;
    for (unsigned i = 0; i < g_handle_count; i++) all[i] = g_handles[i].handle;
    *count = g_handle_count;
    *buffer = all;
    return EFI_SUCCESS;
  }
  UINTN matches = 0;
  for (unsigned i = 0; i < g_handle_count; i++)
    if (HandleFind(&g_handles[i], guid)) matches++;
  if (!matches) return EFI_NOT_FOUND;
  EFI_HANDLE *list = calloc(matches, sizeof(EFI_HANDLE));
  if (!list) return EFI_OUT_OF_RESOURCES;
  UINTN at = 0;
  for (unsigned i = 0; i < g_handle_count; i++)
    if (HandleFind(&g_handles[i], guid)) list[at++] = g_handles[i].handle;
  *count = matches;
  *buffer = list;
  return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI SimLocateProtocol(EFI_GUID *guid, VOID *registration,
                                           VOID **iface) {
  (void)registration;
  if (!guid || !iface) return EFI_INVALID_PARAMETER;
  *iface = NULL;
  for (unsigned i = 0; i < g_handle_count; i++) {
    void *found = HandleFind(&g_handles[i], guid);
    if (found) { *iface = found; return EFI_SUCCESS; }
  }
  return EFI_NOT_FOUND;
}

/* The one BY_DRIVER relationship this machine publishes: the stock display
 * driver owns the target's PCI I/O protocol. Everything the release path
 * inspects downstream (driver binding, loaded image) hangs off these. */
static SIM_OPEN_RECORD g_open_records[SIM_MAX_OPEN_RECORDS];
static unsigned g_open_record_count;

/* A driver unbind removes its BY_DRIVER claims from the graph. */
static void OpenRecordDrop(EFI_HANDLE handle, EFI_HANDLE agent) {
  unsigned keep = 0;
  for (unsigned i = 0; i < g_open_record_count; i++) {
    if (g_open_records[i].handle == handle &&
        (!agent || g_open_records[i].agent == agent))
      continue;
    g_open_records[keep++] = g_open_records[i];
  }
  g_open_record_count = keep;
}

static void OpenRecordAdd(EFI_HANDLE handle, EFI_GUID *guid, EFI_HANDLE agent,
                          EFI_HANDLE controller, UINT32 attributes,
                          UINT32 open_count) {
  if (g_open_record_count >= SIM_MAX_OPEN_RECORDS) return;
  SIM_OPEN_RECORD *r = &g_open_records[g_open_record_count++];
  r->handle = handle;
  r->guid = *guid;
  r->agent = agent;
  r->controller = controller;
  r->attributes = attributes;
  r->open_count = open_count;
}

static EFI_STATUS EFIAPI SimOpenProtocolInformation(EFI_HANDLE handle,
                                                    EFI_GUID *guid,
                                                    EFI_OPEN_PROTOCOL_INFORMATION_ENTRY **entries,
                                                    UINTN *count) {
  if (!guid || !entries || !count) return EFI_INVALID_PARAMETER;
  *entries = NULL;
  *count = 0;
  SIM_HANDLE *h = HandleOf(handle);
  if (!h || !HandleFind(h, guid)) return EFI_UNSUPPORTED;
  UINTN matches = 0;
  for (unsigned i = 0; i < g_open_record_count; i++)
    if (g_open_records[i].handle == handle &&
        memcmp(&g_open_records[i].guid, guid, sizeof(EFI_GUID)) == 0)
      matches++;
  if (!matches) return EFI_SUCCESS;
  EFI_OPEN_PROTOCOL_INFORMATION_ENTRY *list =
      calloc(matches, sizeof(*list));
  if (!list) return EFI_OUT_OF_RESOURCES;
  UINTN at = 0;
  for (unsigned i = 0; i < g_open_record_count; i++) {
    SIM_OPEN_RECORD *r = &g_open_records[i];
    if (r->handle != handle || memcmp(&r->guid, guid, sizeof(EFI_GUID)) != 0)
      continue;
    list[at].AgentHandle = r->agent;
    list[at].ControllerHandle = r->controller;
    list[at].Attributes = r->attributes;
    list[at].OpenCount = r->open_count;
    at++;
  }
  *entries = list;
  *count = matches;
  return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI SimProtocolsPerHandle(EFI_HANDLE handle,
                                               EFI_GUID ***protocols,
                                               UINTN *count) {
  if (!protocols || !count) return EFI_INVALID_PARAMETER;
  *protocols = NULL;
  *count = 0;
  SIM_HANDLE *h = HandleOf(handle);
  if (!h || !h->count) return EFI_NOT_FOUND;
  EFI_GUID **list = calloc(h->count, sizeof(*list));
  if (!list) return EFI_OUT_OF_RESOURCES;
  for (unsigned i = 0; i < h->count; i++)
    list[i] = &h->protocols[i].guid;
  *protocols = list;
  *count = h->count;
  return EFI_SUCCESS;
}

/* Only the driver-model open modes become visible relationships; BY_HANDLE,
 * GET_PROTOCOL and TEST_PROTOCOL leave no trace in the graph. */
static EFI_STATUS EFIAPI SimOpenProtocol(EFI_HANDLE handle, EFI_GUID *guid,
                                         VOID **iface, EFI_HANDLE agent,
                                         EFI_HANDLE controller,
                                         UINT32 attributes) {
  if (!guid || !agent) return EFI_INVALID_PARAMETER;
  EFI_STATUS status = SimHandleProtocol(handle, guid, iface);
  if (status != EFI_SUCCESS) return status;
  if (attributes == EFI_OPEN_PROTOCOL_BY_DRIVER ||
      attributes == EFI_OPEN_PROTOCOL_BY_CHILD_CONTROLLER ||
      attributes == EFI_OPEN_PROTOCOL_EXCLUSIVE)
    OpenRecordAdd(handle, guid, agent, controller, attributes, 1);
  return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI SimCloseProtocol(EFI_HANDLE handle, EFI_GUID *guid,
                                          EFI_HANDLE agent,
                                          EFI_HANDLE controller) {
  if (!guid || !agent) return EFI_INVALID_PARAMETER;
  unsigned keep = 0;
  for (unsigned i = 0; i < g_open_record_count; i++) {
    SIM_OPEN_RECORD *r = &g_open_records[i];
    if (r->handle == handle && r->agent == agent &&
        (r->controller == controller || !controller) &&
        memcmp(&r->guid, guid, sizeof(EFI_GUID)) == 0)
      continue;
    g_open_records[keep++] = *r;
  }
  g_open_record_count = keep;
  return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI SimDisconnectController(EFI_HANDLE controller,
                                                 EFI_HANDLE driver,
                                                 EFI_HANDLE child) {
  (void)driver;
  (void)child;
  g_stats.disconnect_calls++;
  if (g_gpu && controller == SimGpuHandle(g_gpu)) {
    OpenRecordDrop(controller, driver ? driver : g_driver_handle);
    SimGpuStockDriverDisconnected(g_gpu);
    return EFI_SUCCESS;
  }
  return EFI_NOT_FOUND;
}

static EFI_STATUS EFIAPI SimConnectController(EFI_HANDLE controller,
                                              EFI_HANDLE *drivers,
                                              EFI_DEVICE_PATH_PROTOCOL *path,
                                              BOOLEAN recursive) {
  (void)controller;
  (void)drivers;
  (void)path;
  (void)recursive;
  g_stats.connect_calls++;
  return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI SimStall(UINTN microseconds) {
  g_stats.stall_calls++;
  g_stats.stalled_us += microseconds;
  return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI SimSetWatchdog(UINTN timeout, UINT64 code,
                                        UINTN data_size, CHAR16 *data) {
  (void)timeout;
  (void)code;
  (void)data_size;
  (void)data;
  return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI SimGetMemoryMap(UINTN *size, EFI_MEMORY_DESCRIPTOR *map,
                                         UINTN *key, UINTN *desc_size,
                                         UINT32 *desc_version) {
  if (size) *size = 0;
  if (map) memset(map, 0, sizeof(*map));
  if (key) *key = 1;
  if (desc_size) *desc_size = sizeof(EFI_MEMORY_DESCRIPTOR);
  if (desc_version) *desc_version = 1;
  return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI SimLoadImage(BOOLEAN boot_policy, EFI_HANDLE parent,
                                      EFI_DEVICE_PATH_PROTOCOL *path,
                                      VOID *source, UINTN source_size,
                                      EFI_HANDLE *image) {
  (void)boot_policy;
  (void)parent;
  (void)path;
  (void)source;
  (void)source_size;
  g_stats.load_image_calls++;
  if (!image) return EFI_INVALID_PARAMETER;
  SIM_HANDLE *h = HandleNew("loaded-image");
  if (!h) return EFI_OUT_OF_RESOURCES;
  *image = h->handle;
  return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI SimStartImage(EFI_HANDLE image, UINTN *exit_size,
                                       CHAR16 **exit_data) {
  (void)image;
  if (exit_size) *exit_size = 0;
  if (exit_data) *exit_data = NULL;
  return EFI_UNSUPPORTED;
}

static EFI_TPL EFIAPI SimRaiseTPL(EFI_TPL level) { return level ? level - 1 : 0; }
static VOID EFIAPI SimRestoreTPL(EFI_TPL level) { (void)level; }

static EFI_STATUS EFIAPI SimCreateEventEx(UINT32 type, EFI_TPL tpl,
                                          EFI_EVENT_NOTIFY notify,
                                          const void *context,
                                          const EFI_GUID *group,
                                          EFI_EVENT *event) {
  (void)type; (void)tpl; (void)notify; (void)context; (void)group;
  if (!event) return EFI_INVALID_PARAMETER;
  static UINTN fake;
  *event = (EFI_EVENT)&fake;
  return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI SimCreateEvent(UINT32 type, EFI_TPL tpl,
                                        EFI_EVENT_NOTIFY notify, VOID *context,
                                        EFI_EVENT *event) {
  return SimCreateEventEx(type, tpl, notify, context, NULL, event);
}

static EFI_STATUS EFIAPI SimCloseEvent(EFI_EVENT event) {
  (void)event;
  return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI SimSetTimer(EFI_EVENT event, EFI_TIMER_DELAY type,
                                     UINT64 trigger) {
  (void)event; (void)type; (void)trigger;
  return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI SimWaitForEvent(UINTN count, EFI_EVENT *events,
                                         UINTN *index) {
  (void)count; (void)events;
  if (index) *index = 0;
  return EFI_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* Runtime services                                                    */
/* ------------------------------------------------------------------ */

/* The platform variables that name the active text console. On the modelled
 * board the console runs on the discrete-survivor AMD output, exactly as the
 * release path expects; the value is an EFI device-path list. */
static const EFI_GUID g_global_variable_guid = {
    0x8be4df61, 0x93ca, 0x11d2, {0xaa, 0x0d, 0x00, 0xe0, 0x98, 0x03, 0x2b, 0x8c}};

/* Fork-owned variable the image reads to learn which PCI functions it may
 * act on. The model stores the same ASCII text the firmware writes. */
static const EFI_GUID g_gu_targets_guid = {
    0x5c1f2b40, 0x8e21, 0x4ef7, {0x9d, 0x2b, 0x7a, 0x11, 0x3c, 0x5e, 0x84, 0x02}};
static const char *g_gu_targets;

void SimWorldSetTargets(const char *Ascii) { g_gu_targets = Ascii; }

static int WideEquals(const CHAR16 *left, const char *ascii) {
  if (!left) return 0;
  for (UINTN i = 0;; i++) {
    if ((char)left[i] != ascii[i]) return 0;
    if (!ascii[i]) return 1;
  }
}

static EFI_STATUS EFIAPI SimGetVariable(CHAR16 *name, EFI_GUID *vendor,
                                        UINT32 *attributes, UINTN *size,
                                        VOID *data) {
  if (!name || !vendor || !size) return EFI_INVALID_PARAMETER;
  if (memcmp(vendor, &g_gu_targets_guid, sizeof(EFI_GUID)) == 0) {
    if (!g_gu_targets) return EFI_NOT_FOUND;
    if (!WideEquals(name, "GuTargets")) return EFI_NOT_FOUND;
    UINTN need = strlen(g_gu_targets);
    if (*size < need) {
      *size = need;
      return EFI_BUFFER_TOO_SMALL;
    }
    if (!data) return EFI_INVALID_PARAMETER;
    memcpy(data, g_gu_targets, need);
    *size = need;
    if (attributes)
      *attributes = EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS;
    return EFI_SUCCESS;
  }
  if (memcmp(vendor, &g_global_variable_guid, sizeof(EFI_GUID)) != 0 ||
      !g_amd_path)
    return EFI_NOT_FOUND;
  if (!WideEquals(name, "ConOut") && !WideEquals(name, "ErrOut"))
    return EFI_NOT_FOUND;
  if (*size < g_amd_path_length) {
    *size = g_amd_path_length;
    return EFI_BUFFER_TOO_SMALL;
  }
  if (!data) return EFI_INVALID_PARAMETER;
  memcpy(data, g_amd_path, g_amd_path_length);
  *size = g_amd_path_length;
  if (attributes) *attributes = EFI_VARIABLE_BOOTSERVICE_ACCESS;
  return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI SimSetVariable(CHAR16 *name, EFI_GUID *vendor,
                                        UINT32 attributes, UINTN size,
                                        VOID *data) {
  (void)name; (void)vendor; (void)attributes; (void)size; (void)data;
  return EFI_SUCCESS;
}

static VOID EFIAPI SimResetSystem(EFI_RESET_TYPE type, EFI_STATUS status,
                                  UINTN size, VOID *data) {
  (void)type; (void)size; (void)data;
  g_stats.reset_calls++;
  SimReportLine("SIM_EVENT=RESET_SYSTEM STATUS=%llx",
                (unsigned long long)status);
}

/* ------------------------------------------------------------------ */
/* World construction                                                  */
/* ------------------------------------------------------------------ */

static void BuildDevicePath(UINT8 *buffer, const UINT8 *guid16, int usb,
                            UINTN *length) {
  UINTN at = 0;
  /* One GPT hard-drive node (42 bytes), matching the parser in
   * src/gpu_windows_path.c. */
  buffer[at + 0] = MEDIA_DEVICE_PATH;
  buffer[at + 1] = MEDIA_HARDDRIVE_DP;
  buffer[at + 2] = 42;
  buffer[at + 3] = 0;
  buffer[at + 4] = 1;              /* partition number */
  buffer[at + 8] = 0x00;           /* partition start (LBA 2048) */
  buffer[at + 9] = 0x08;
  buffer[at + 12] = 0xff;          /* partition end (open ended) */
  buffer[at + 13] = 0xff;
  memcpy(buffer + at + 24, guid16, 16);
  buffer[at + 40] = 2;             /* GPT signature */
  buffer[at + 41] = 2;
  at += 42;
  if (usb) {
    buffer[at + 0] = MESSAGING_DEVICE_PATH;
    buffer[at + 1] = MSG_USB_DP;
    buffer[at + 2] = 6;
    buffer[at + 3] = 0;
    buffer[at + 4] = 1;            /* parent port */
    buffer[at + 5] = 1;            /* interface */
    at += 6;
  }
  buffer[at + 0] = END_DEVICE_PATH_TYPE;
  buffer[at + 1] = END_ENTIRE_DEVICE_PATH_SUBTYPE;
  buffer[at + 2] = 4;
  buffer[at + 3] = 0;
  at += 4;
  *length = at;
}

/* PCI root + PCI function device path, the shape a UEFI PCI bus driver
 * produces. Two adapters differ only in the PCI node, so a prefix test
 * distinguishes the target from a surviving display output. */
static EFI_STATUS SimPciDevicePath(UINT8 device, UINT8 function,
                                   UINT8 **out, UINTN *length) {
  if (!out || !length) return EFI_INVALID_PARAMETER;
  UINT8 *buffer = calloc(1, 32);
  if (!buffer) return EFI_OUT_OF_RESOURCES;
  UINTN at = 0;
  buffer[at + 0] = ACPI_DEVICE_PATH;
  buffer[at + 1] = ACPI_DP;
  buffer[at + 2] = 12;
  buffer[at + 3] = 0;
  buffer[at + 4] = 0xd0;           /* PNP0A03 (PCI root bridge) */
  buffer[at + 5] = 0x41;
  buffer[at + 6] = 0x03;
  buffer[at + 7] = 0x0a;
  buffer[at + 8] = 0;              /* UID */
  buffer[at + 9] = 0;
  buffer[at + 10] = 0;
  buffer[at + 11] = 0;
  at += 12;
  buffer[at + 0] = HARDWARE_DEVICE_PATH;
  buffer[at + 1] = HW_PCI_DP;
  buffer[at + 2] = 6;
  buffer[at + 3] = 0;
  buffer[at + 4] = function;
  buffer[at + 5] = device;
  at += 6;
  buffer[at + 0] = END_DEVICE_PATH_TYPE;
  buffer[at + 1] = END_ENTIRE_DEVICE_PATH_SUBTYPE;
  buffer[at + 2] = 4;
  buffer[at + 3] = 0;
  at += 4;
  *out = buffer;
  *length = at;
  return EFI_SUCCESS;
}

static SIM_VOLUME *VolumeAdd(const char *name, const char *host_root,
                            const UINT8 *guid16, int usb, int writable) {
  if (g_volume_count >= SIM_MAX_VOLUMES) return NULL;
  SIM_VOLUME *volume = &g_volumes[g_volume_count];
  memset(volume, 0, sizeof(*volume));
  volume->host_root = host_root;
  volume->writable = writable;
  volume->is_usb = usb;
  volume->path_bytes = calloc(1, 256);
  if (!volume->path_bytes) return NULL;
  BuildDevicePath(volume->path_bytes, guid16, usb, &volume->path_length);
  volume->path = (EFI_DEVICE_PATH_PROTOCOL *)volume->path_bytes;
  volume->fs.Revision = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_REVISION;
  volume->fs.OpenVolume = SimVolumeOpen;
  volume->fs_ptr = &volume->fs;
  SIM_HANDLE *h = HandleNew(name);
  if (!h) return NULL;
  volume->handle = h->handle;
  static EFI_GUID fs_guid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
  static EFI_GUID path_guid = EFI_DEVICE_PATH_PROTOCOL_GUID;
  HandleAdd(h, &fs_guid, &volume->fs);
  HandleAdd(h, &path_guid, volume->path);
  g_volume_count++;
  return volume;
}

/* ------------------------------------------------------------------ */
/* The AMD display function the console runs on                       */
/* ------------------------------------------------------------------ */
/* Only the parts the release path actually reads: identity registers and a
 * location. The board is a hybrid laptop/desktop where the NVIDIA card is
 * headless and ConOut lives on this AMD function. */
typedef struct {
  EFI_PCI_IO_PROTOCOL io;
  UINTN  segment, bus, device, function;
  UINT8 *config;
} SIM_AMD_PCI;

static UINTN WidthBytes(EFI_PCI_IO_PROTOCOL_WIDTH width) {
  switch (width) {
    case EfiPciIoWidthUint8: return 1;
    case EfiPciIoWidthUint16: return 2;
    case EfiPciIoWidthUint32: return 4;
    case EfiPciIoWidthUint64: return 8;
    default: return 0;
  }
}

static SIM_AMD_PCI g_amd;

static EFI_STATUS EFIAPI SimAmdPciRead(EFI_PCI_IO_PROTOCOL *self,
                                       EFI_PCI_IO_PROTOCOL_WIDTH width,
                                       UINT32 offset, UINTN count, VOID *buffer) {
  SIM_AMD_PCI *amd = (SIM_AMD_PCI *)((UINT8 *)self - offsetof(SIM_AMD_PCI, io));
  UINTN unit = WidthBytes(width);
  if (!buffer || !unit) return EFI_INVALID_PARAMETER;
  if (offset > SIM_CONFIG_BYTES || count > (SIM_CONFIG_BYTES - offset) / unit)
    return EFI_INVALID_PARAMETER;
  memcpy(buffer, amd->config + offset, count * unit);
  return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI SimAmdPciWrite(EFI_PCI_IO_PROTOCOL *self,
                                        EFI_PCI_IO_PROTOCOL_WIDTH width,
                                        UINT32 offset, UINTN count, VOID *buffer) {
  SIM_AMD_PCI *amd = (SIM_AMD_PCI *)((UINT8 *)self - offsetof(SIM_AMD_PCI, io));
  UINTN unit = WidthBytes(width);
  if (!buffer || !unit) return EFI_INVALID_PARAMETER;
  if (offset > SIM_CONFIG_BYTES || count > (SIM_CONFIG_BYTES - offset) / unit)
    return EFI_INVALID_PARAMETER;
  memcpy(amd->config + offset, buffer, count * unit);
  return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI SimAmdGetLocation(EFI_PCI_IO_PROTOCOL *self,
                                           UINTN *segment, UINTN *bus,
                                           UINTN *device, UINTN *function) {
  SIM_AMD_PCI *amd = (SIM_AMD_PCI *)((UINT8 *)self - offsetof(SIM_AMD_PCI, io));
  if (segment) *segment = amd->segment;
  if (bus) *bus = amd->bus;
  if (device) *device = amd->device;
  if (function) *function = amd->function;
  return EFI_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* Firmware graph the release path inspects                           */
/* ------------------------------------------------------------------ */
/* A headless target still needs one non-target display output, one
 * client-loaded stock driver with a real binding, and the BY_DRIVER
 * relationship that the preflight snapshot must see. */

static EFI_GRAPHICS_OUTPUT_PROTOCOL        g_gop;
static EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE   g_gop_mode;
static EFI_GRAPHICS_OUTPUT_MODE_INFORMATION g_gop_info;
static UINT8                              *g_driver_image_bytes;
static EFI_LOADED_IMAGE_PROTOCOL           g_driver_loaded;
static EFI_DRIVER_BINDING_PROTOCOL         g_driver_binding;

#define SIM_DRIVER_IMAGE_BYTES 0x10000u

/* The binding's Stop must fall inside its own image, exactly as a loaded
 * driver's does. The image is synthetic, so Stop is modelled by placing the
 * address of a real no-op inside the anonymous region's window. */
static EFI_STATUS SimWorldBuildFirmwareGraph(void) {
  static EFI_GUID gop_guid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
  static EFI_GUID path_guid = EFI_DEVICE_PATH_PROTOCOL_GUID;
  static EFI_GUID binding_guid = EFI_DRIVER_BINDING_PROTOCOL_GUID;
  static EFI_GUID image_guid = EFI_LOADED_IMAGE_PROTOCOL_GUID;

  /* Non-target display output (the AMD function) so the target is headless. */
  SIM_HANDLE *gop_handle = HandleNew("amd-display");
  if (!gop_handle) return EFI_OUT_OF_RESOURCES;
  UINT8 *gop_path = NULL;
  UINTN gop_path_length = 0;
  if (SimPciDevicePath(2, 0, &gop_path, &gop_path_length) != EFI_SUCCESS)
    return EFI_DEVICE_ERROR;
  g_amd_path = gop_path;
  g_amd_path_length = gop_path_length;
  g_amd.segment = 0;
  g_amd.bus = 0;
  g_amd.device = 2;
  g_amd.function = 0;
  g_amd.config = calloc(1, SIM_CONFIG_BYTES);
  if (!g_amd.config) return EFI_OUT_OF_RESOURCES;
  g_amd.config[0x00] = 0x02;               /* AMD */
  g_amd.config[0x01] = 0x10;
  g_amd.config[0x02] = 0x4e;               /* Rembrandt-class iGPU */
  g_amd.config[0x03] = 0x16;
  g_amd.config[0x0a] = 0x00;               /* display controller */
  g_amd.config[0x0b] = 0x03;
  g_amd.io.Pci.Read = SimAmdPciRead;
  g_amd.io.Pci.Write = SimAmdPciWrite;
  g_amd.io.GetLocation = SimAmdGetLocation;
  g_gop_info.Version = 1;
  g_gop_info.HorizontalResolution = 1024;
  g_gop_info.VerticalResolution = 768;
  g_gop_info.PixelFormat = PixelBlueGreenRedReserved8BitPerColor;
  g_gop_info.PixelsPerScanLine = 1024;
  g_gop_mode.MaxMode = 1;
  g_gop_mode.Mode = 0;
  g_gop_mode.Info = &g_gop_info;
  g_gop_mode.SizeOfInfo = sizeof(g_gop_info);
  g_gop_mode.FrameBufferSize = 1024 * 768 * 4;
  g_gop_mode.FrameBufferBase = 0xfd000000ull;
  g_gop.Mode = &g_gop_mode;
  static EFI_GUID pci_io_guid = EFI_PCI_IO_PROTOCOL_GUID;
  static EFI_GUID text_guid = EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL_GUID;
  HandleAdd(gop_handle, &pci_io_guid, &g_amd.io);
  HandleAdd(gop_handle, &gop_guid, &g_gop);
  HandleAdd(gop_handle, &path_guid, gop_path);
  (void)gop_path_length;
  /* The console splitter's child terminal for that AMD function: the child
   * handle re-publishes TextOutput, which ConsoleOutHandle opens BY_CHILD.
   * ConsoleOutHandle also backs StdErr, so one edge satisfies both. */
  HandleAdd(gop_handle, &text_guid, &g_conout);
  OpenRecordAdd(gop_handle->handle, &text_guid, g_console_handle,
                g_console_handle, EFI_OPEN_PROTOCOL_BY_CHILD_CONTROLLER, 1);

  /* Stock display driver: loaded image + binding + one BY_DRIVER claim. */
  g_driver_image_bytes = mmap(NULL, SIM_DRIVER_IMAGE_BYTES, PROT_READ | PROT_WRITE,
                              MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (g_driver_image_bytes == MAP_FAILED) {
    g_driver_image_bytes = NULL;
    return EFI_OUT_OF_RESOURCES;
  }
  SIM_HANDLE *image = HandleNew("stock-driver-image");
  SIM_HANDLE *driver = HandleNew("stock-driver");
  if (!image || !driver) return EFI_OUT_OF_RESOURCES;
  g_driver_handle = driver->handle;
  g_driver_loaded.Revision = EFI_LOADED_IMAGE_PROTOCOL_REVISION;
  g_driver_loaded.ParentHandle = g_image_handle;
  g_driver_loaded.ImageBase = g_driver_image_bytes;
  g_driver_loaded.ImageSize = SIM_DRIVER_IMAGE_BYTES;
  g_driver_loaded.ImageCodeType = EfiBootServicesCode;
  g_driver_loaded.ImageDataType = EfiLoaderData;
  g_driver_binding.Version = 0x00000010;
  g_driver_binding.ImageHandle = image->handle;
  g_driver_binding.DriverBindingHandle = driver->handle;
  g_driver_binding.Supported = NULL;
  g_driver_binding.Start = NULL;
  {
    /* Stop must lie inside the driver's own image, as a loaded driver's does.
     * The image is synthetic, so the entry point is modelled by an address in
     * its window; the world's DisconnectController performs the unbind. */
    union {
      UINT8                  *address;
      EFI_DRIVER_BINDING_STOP stop;
    } entry;
    entry.address = g_driver_image_bytes + 0x100;
    g_driver_binding.Stop = entry.stop;
  }
  HandleAdd(image, &image_guid, &g_driver_loaded);
  HandleAdd(driver, &binding_guid, &g_driver_binding);
  HandleAdd(driver, &image_guid, &g_driver_loaded);

  /* The target's PCI I/O protocol is open BY_DRIVER by that stock driver. */
  static EFI_GUID pci_guid = EFI_PCI_IO_PROTOCOL_GUID;
  OpenRecordAdd(SimGpuHandle(g_gpu), &pci_guid, driver->handle,
                SimGpuHandle(g_gpu), EFI_OPEN_PROTOCOL_BY_DRIVER, 1);
  return EFI_SUCCESS;
}

EFI_STATUS SimWorldInit(const char *seed_dir, const char *esp_dir,
                        const char *windows_dir) {
  (void)esp_dir;
  memset(&g_stats, 0, sizeof(g_stats));
  memset(g_handles, 0, sizeof(g_handles));
  memset(g_volumes, 0, sizeof(g_volumes));
  memset(g_files, 0, sizeof(g_files));
  g_handle_count = g_volume_count = 0;
  g_log_used = 0;
  g_log[0] = 0;

  g_bs.Hdr.Signature = EFI_BOOT_SERVICES_SIGNATURE;
  g_bs.Hdr.Revision = EFI_BOOT_SERVICES_REVISION;
  g_bs.Hdr.HeaderSize = sizeof(EFI_BOOT_SERVICES);
  g_bs.RaiseTPL = SimRaiseTPL;
  g_bs.RestoreTPL = SimRestoreTPL;
  g_bs.AllocatePool = SimAllocatePool;
  g_bs.FreePool = SimFreePool;
  g_bs.CreateEvent = SimCreateEvent;
  g_bs.CreateEventEx = SimCreateEventEx;
  g_bs.CloseEvent = SimCloseEvent;
  g_bs.SetTimer = SimSetTimer;
  g_bs.WaitForEvent = SimWaitForEvent;
  g_bs.LocateHandleBuffer = SimLocateHandleBuffer;
  g_bs.HandleProtocol = SimHandleProtocol;
  g_bs.LocateProtocol = SimLocateProtocol;
  g_bs.OpenProtocol = SimOpenProtocol;
  g_bs.CloseProtocol = SimCloseProtocol;
  g_bs.OpenProtocolInformation = SimOpenProtocolInformation;
  g_bs.ProtocolsPerHandle = SimProtocolsPerHandle;
  g_bs.ConnectController = SimConnectController;
  g_bs.DisconnectController = SimDisconnectController;
  g_bs.Stall = SimStall;
  g_bs.SetWatchdogTimer = SimSetWatchdog;
  g_bs.GetMemoryMap = SimGetMemoryMap;
  g_bs.LoadImage = SimLoadImage;
  g_bs.StartImage = SimStartImage;

  g_rt.Hdr.Signature = EFI_RUNTIME_SERVICES_SIGNATURE;
  g_rt.Hdr.Revision = EFI_RUNTIME_SERVICES_REVISION;
  g_rt.Hdr.HeaderSize = sizeof(EFI_RUNTIME_SERVICES);
  g_rt.GetVariable = SimGetVariable;
  g_rt.SetVariable = SimSetVariable;
  g_rt.ResetSystem = SimResetSystem;

  g_conout.Reset = SimOutReset;
  g_conout.OutputString = SimOutString;
  g_conin.Reset = SimInReset;
  g_conin.ReadKeyStroke = SimInReadKey;
  /* The key event is a plain handle: blocking waits are not modelled, the
   * harness queues keystrokes before it runs the image. */
  static UINTN conin_event;
  g_conin.WaitForKey = (EFI_EVENT)&conin_event;

  SIM_HANDLE *conout = HandleNew("console-out");
  SIM_HANDLE *conin = HandleNew("console-in");
  if (!conout || !conin) return EFI_OUT_OF_RESOURCES;
  static EFI_GUID conout_guid = EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL_GUID;
  static EFI_GUID conin_guid = EFI_SIMPLE_TEXT_INPUT_PROTOCOL_GUID;
  HandleAdd(conout, &conout_guid, &g_conout);
  HandleAdd(conin, &conin_guid, &g_conin);

  g_st.Hdr.Signature = EFI_SYSTEM_TABLE_SIGNATURE;
  g_st.Hdr.Revision = EFI_SYSTEM_TABLE_REVISION;
  g_st.Hdr.HeaderSize = sizeof(EFI_SYSTEM_TABLE);
  g_st.BootServices = &g_bs;
  g_st.RuntimeServices = &g_rt;
  g_st.ConOut = &g_conout;
  g_st.ConIn = &g_conin;
  g_st.StdErr = &g_conout;
  g_st.ConsoleOutHandle = conout->handle;
  g_console_handle = conout->handle;
  g_st.ConsoleInHandle = conin->handle;
  g_st.StandardErrorHandle = conout->handle;

  /* ESP GUIDs: one for the boot media, one for the pinned Windows ESP.  The
   * Windows value must equal GU_WINDOWS_PARTITION_INITIALIZER. */
  static const UINT8 media_guid[16] = {0x11,0x22,0x33,0x44,0x55,0x66,0x77,0x88,
                                       0x99,0xaa,0xbb,0xcc,0xdd,0xee,0x00,0x01};
  const EFI_GUID windows = GU_WINDOWS_PARTITION_INITIALIZER;
  UINT8 windows_bytes[16];
  memcpy(windows_bytes, &windows, 16);

  if (!VolumeAdd("esp-volume", seed_dir, media_guid, 1, 1))
    return EFI_OUT_OF_RESOURCES;
  if (windows_dir && !VolumeAdd("windows-volume", windows_dir, windows_bytes, 0, 0))
    return EFI_OUT_OF_RESOURCES;

  /* Image handle: the running EFI application lives on the USB volume. */
  SIM_HANDLE *image = HandleNew("image");
  if (!image) return EFI_OUT_OF_RESOURCES;
  g_image_handle = image->handle;
  static EFI_LOADED_IMAGE_PROTOCOL loaded;
  memset(&loaded, 0, sizeof(loaded));
  loaded.Revision = EFI_LOADED_IMAGE_PROTOCOL_REVISION;
  loaded.SystemTable = &g_st;
  loaded.DeviceHandle = g_volumes[0].handle;
  static EFI_GUID loaded_guid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
  HandleAdd(image, &loaded_guid, &loaded);

  if (SimGpuBuild(&g_gpu) != EFI_SUCCESS) return EFI_DEVICE_ERROR;
  static EFI_GUID pci_guid = EFI_PCI_IO_PROTOCOL_GUID;
  EFI_PCI_IO_PROTOCOL *pci_io = SimGpuIo(g_gpu);
  if (!pci_io) return EFI_DEVICE_ERROR;
  static EFI_GUID gpu_path_guid = EFI_DEVICE_PATH_PROTOCOL_GUID;
  UINT8 *gpu_path = NULL;
  UINTN gpu_path_length = 0;
  if (SimPciDevicePath(0, 0, &gpu_path, &gpu_path_length) != EFI_SUCCESS)
    return EFI_DEVICE_ERROR;
  (void)gpu_path_length;
  HandleAdd(HandleOf(SimGpuHandle(g_gpu)), &pci_guid, pci_io);
  HandleAdd(HandleOf(SimGpuHandle(g_gpu)), &gpu_path_guid, gpu_path);
  return SimWorldBuildFirmwareGraph();
}

void SimWorldShutdown(void) {
  if (g_gpu) SimGpuFree(g_gpu);
  g_gpu = NULL;
  for (unsigned i = 0; i < g_volume_count; i++) free(g_volumes[i].path_bytes);
  g_volume_count = 0;
}

EFI_HANDLE SimRegisterGpuHandle(const char *name) {
  SIM_HANDLE *h = HandleNew(name);
  return h ? h->handle : NULL;
}

EFI_SYSTEM_TABLE *SimSystemTable(void) { return &g_st; }
EFI_HANDLE SimImageHandle(void) { return g_image_handle; }
SIM_GPU *SimPrimaryGpu(void) { return g_gpu; }
SIM_STATS *SimStats(void) { return &g_stats; }

SIM_VOLUME *SimEspVolume(void) {
  for (unsigned i = 0; i < g_volume_count; i++)
    if (g_volumes[i].is_usb) return &g_volumes[i];
  return NULL;
}

SIM_VOLUME *SimWindowsVolume(void) {
  for (unsigned i = 0; i < g_volume_count; i++)
    if (!g_volumes[i].is_usb) return &g_volumes[i];
  return NULL;
}

void SimReportLine(const char *format, ...) {
  char buffer[1024];
  va_list args;
  va_start(args, format);
  vsnprintf(buffer, sizeof(buffer), format, args);
  va_end(args);
  printf("%s\n", buffer);
  fflush(stdout);
}

void SimFail(const char *format, ...) {
  char buffer[1024];
  va_list args;
  va_start(args, format);
  vsnprintf(buffer, sizeof(buffer), format, args);
  va_end(args);
  g_failures++;
  printf("SIM_CHECK=FAIL %s\n", buffer);
  fflush(stdout);
}

int SimFailures(void) { return g_failures; }
