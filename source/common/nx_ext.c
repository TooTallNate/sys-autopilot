#include "nx_ext.h"

#ifdef __SWITCH__
#include <string.h>

// --- ns: ApplicationManagerInterface ----------------------------------------

static Service g_ns_app;
static bool g_ns_app_ready;

Result nsext_init(void) {
    if (g_ns_app_ready)
        return 0;
    Result rc = nsInitialize();
    if (R_FAILED(rc))
        return rc;
    // On 3.0.0+ the record commands live behind GetApplicationManagerInterface;
    // earlier the top-level ns:am session is used directly.
    if (hosversionAtLeast(3, 0, 0))
        rc = nsGetApplicationManagerInterface(&g_ns_app);
    else
        g_ns_app = *nsGetServiceSession_ApplicationManagerInterface();
    if (R_FAILED(rc)) {
        nsExit();
        return rc;
    }
    g_ns_app_ready = true;
    return 0;
}

void nsext_exit(void) {
    if (!g_ns_app_ready)
        return;
    serviceClose(&g_ns_app);
    nsExit();
    g_ns_app_ready = false;
}

// ApplicationRecordType_Installed
#define NSEXT_RECORD_INSTALLED 3

Result nsext_push_application_record(u64 app_id,
                                     const NsExtContentStorageRecord *records,
                                     u32 count) {
    const struct {
        u8  last_modified_event;
        u8  padding[7];
        u64 app_id;
    } in = { NSEXT_RECORD_INSTALLED, {0}, app_id };

    return serviceDispatchIn(&g_ns_app, 16, in,
        .buffer_attrs = { SfBufferAttr_HipcMapAlias | SfBufferAttr_In },
        .buffers = { { records, sizeof(*records) * count } });
}

Result nsext_list_application_record_content_meta(u64 offset, u64 app_id,
                                                  NsExtContentStorageRecord *out,
                                                  u32 count, s32 *out_count) {
    const struct {
        u64 offset;
        u64 app_id;
    } in = { offset, app_id };

    return serviceDispatchInOut(&g_ns_app, 17, in, *out_count,
        .buffer_attrs = { SfBufferAttr_HipcMapAlias | SfBufferAttr_Out },
        .buffers = { { out, sizeof(*out) * count } });
}

Result nsext_delete_application_record(u64 app_id) {
    return serviceDispatchIn(&g_ns_app, 27, app_id);
}

// --- es ----------------------------------------------------------------------

static Service g_es;
static bool g_es_ready;

Result esext_init(void) {
    if (g_es_ready)
        return 0;
    Result rc = smGetService(&g_es, "es");
    if (R_FAILED(rc))
        return rc;
    g_es_ready = true;
    return 0;
}

void esext_exit(void) {
    if (!g_es_ready)
        return;
    serviceClose(&g_es);
    g_es_ready = false;
}

Result esext_import_ticket(const void *tik, u64 tik_size,
                           const void *cert, u64 cert_size) {
    return serviceDispatch(&g_es, 1,
        .buffer_attrs = { SfBufferAttr_HipcMapAlias | SfBufferAttr_In,
                          SfBufferAttr_HipcMapAlias | SfBufferAttr_In },
        .buffers = { { tik, tik_size }, { cert, cert_size } });
}

#endif // __SWITCH__
