#pragma once

// IPC wrappers for ns (application records) and es (ticket import) commands
// that libnx 4.12 does not expose. Needed to register an installed title so it
// appears on the HOME menu and to import its ticket.
//
// Adapted from the implementations in ITotalJustice/sphaira (yati/nx/ns.cpp,
// es.cpp) and XorTroll/Goldleaf; command IDs match the application-manager
// and es interfaces.

#ifdef __SWITCH__
#include <switch.h>

// A content-storage record: a content-meta key plus the storage it lives on.
// Layout matches the struct the ns ApplicationManager record commands expect.
typedef struct {
    NcmContentMetaKey key;
    u8 storage_id; // NcmStorageId
    u8 padding[7];
} NsExtContentStorageRecord;

// ns: open/close the ApplicationManagerInterface used by the record commands.
Result nsext_init(void);
void   nsext_exit(void);

// ns ApplicationManager record management (cmd 16/17/27).
Result nsext_push_application_record(u64 app_id,
                                     const NsExtContentStorageRecord *records,
                                     u32 count);
Result nsext_list_application_record_content_meta(u64 offset, u64 app_id,
                                                  NsExtContentStorageRecord *out,
                                                  u32 count, s32 *out_count);
Result nsext_delete_application_record(u64 app_id);

// es: open/close and import a ticket (+ cert) via cmd 1.
Result esext_init(void);
void   esext_exit(void);
Result esext_import_ticket(const void *tik, u64 tik_size,
                           const void *cert, u64 cert_size);
#endif
