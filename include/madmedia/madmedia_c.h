#ifndef MADMEDIA_C_H
#define MADMEDIA_C_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define MADMEDIA_ABI_VERSION 1u /* v1 is frozen */
#define MADMEDIA_CONFIG_V2_VERSION 2u
#define MADMEDIA_TASK_ID_CAPACITY 128u
typedef uint64_t madmedia_handle;
typedef enum { MADMEDIA_OK=0, MADMEDIA_INVALID=1, MADMEDIA_CLOSED=2,
 MADMEDIA_BUSY=3, MADMEDIA_EMPTY=4, MADMEDIA_BUFFER_TOO_SMALL=5,
 MADMEDIA_NATIVE_ERROR=6, MADMEDIA_INTERNAL=7 } madmedia_status;
typedef struct {
 uint32_t struct_size, abi_version;
 const char* base_url;
 const char* data_dir;
 uint32_t max_pending_tasks; /* 1..256; active + unconsumed terminal tasks */
 uint32_t reserved;
} madmedia_config_v1;
typedef enum { MADMEDIA_JOURNAL_NORMAL=0, MADMEDIA_JOURNAL_FULL=1 } madmedia_journal_mode;
typedef enum { MADMEDIA_RECEIPT_NONE=0, MADMEDIA_RECEIPT_NORMAL=1,
 MADMEDIA_RECEIPT_FULL=2, MADMEDIA_RECEIPT_FAILED=3 } madmedia_journal_receipt;
typedef struct {
 uint32_t struct_size, abi_version; /* sizeof(config_v2), 2 */
 const char* base_url;
 const char* data_dir;
 uint32_t max_pending_tasks; /* same 1..256 admission limit */
 uint32_t reserved; /* must be zero */
 uint32_t journal_mode; /* madmedia_journal_mode */
 uint32_t reserved2; /* must be zero */
} madmedia_config_v2;
/* All strings are caller-owned UTF-8, borrowed for call only. Never throws C++.
 * Calls may run concurrently; handle destruction serializes against operations.
 * No host callbacks: poll on an application executor, not the UI thread.
 * Task submission requires an output buffer of at least 128 bytes BEFORE it
 * causes any side effects. Do not probe submit by executing it twice.
 */
uint32_t madmedia_abi_version(void);
madmedia_status madmedia_create(const madmedia_config_v1*, madmedia_handle* out);
/* Additive v2 config; v1 structs, version query and JSON remain unchanged.
 * v2 poll JSON includes journal_receipt (0..3) retained across progress
 * coalescing until terminal consumption or destroy. It acknowledges only the
 * initial task-row commit, NOT media bytes or transfer completion. NONE means
 * unknown, not uncommitted. No replay/recovery guarantee. FULL depends on
 * SQLite/VFS/filesystem/device honoring synchronization.
 */
madmedia_status madmedia_create_v2(const madmedia_config_v2*, madmedia_handle* out);
madmedia_status madmedia_upload(madmedia_handle,const char* path,const char* mime,char* task_id,size_t capacity);
madmedia_status madmedia_download(madmedia_handle,const char* media_id,const char* destination,char* task_id,size_t capacity);
madmedia_status madmedia_cancel(madmedia_handle,const char* task_id);
/* Returns a JSON snapshot, NUL included in required_bytes. A size probe or
 * insufficient buffer DOES NOT consume the snapshot. Progress coalesces per
 * task. Ready/Failed/Canceled are pinned until successful poll; capacity is
 * released only then. Failed is terminal at this C facade; no resume in v1.
 * Counter bytes are decimal STRINGS to preserve 64-bit values in JSON.
 * Concurrent polls are safe but size can change between probe and copy: retry
 * BUFFER_TOO_SMALL. Callback-copy failure is sticky INTERNAL, never hidden drop. Poll JSON
 * allocation failure returns INTERNAL without consuming, and may be retried.
 */
madmedia_status madmedia_poll(madmedia_handle,char* json,size_t capacity,size_t* required_bytes);
/* Explicit close abandons unconsumed snapshots; completes native shutdown.
 * Repeated destroy returns CLOSED. No recovery/streaming/provider ABI in v1.
 */
madmedia_status madmedia_destroy(madmedia_handle);
#ifdef __cplusplus
}
#endif
#endif
