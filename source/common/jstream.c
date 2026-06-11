#include "jstream.h"

#include <string.h>

static const char *kTarget[3] = { "params", "arguments", "content" };
static const char kPlaceholder[] = "\"<streamed>\"";

void jstream_init(Jstream *js, char *doc_buf, size_t doc_cap,
                  JstreamSink sink, void *sink_ctx) {
    memset(js, 0, sizeof(*js));
    js->doc = doc_buf;
    js->doc_cap = doc_cap;
    js->sink = sink;
    js->sink_ctx = sink_ctx;
}

static int doc_append(Jstream *js, const char *data, size_t len) {
    if (js->doc_len + len > js->doc_cap)
        return JSTREAM_EDOC;
    memcpy(js->doc + js->doc_len, data, len);
    js->doc_len += len;
    return 0;
}

static int doc_append_ch(Jstream *js, char c) {
    return doc_append(js, &c, 1);
}

// A key string just completed at the current depth: decide whether the
// upcoming value is on the target path / is the content field.
static void key_complete(Jstream *js) {
    int d = js->depth;
    js->next_value_on_path = false;
    js->next_value_is_content = false;
    if (js->key_overflow || d < 1 || d > 3 || !js->on_path[d])
        return;
    js->key[js->key_len] = '\0';
    if (strcmp(js->key, kTarget[d - 1]) != 0)
        return;
    if (d < 3)
        js->next_value_on_path = true;
    else
        js->next_value_is_content = true;
}

int jstream_feed(Jstream *js, const char *data, size_t len) {
    if (js->err)
        return js->err;

    size_t i = 0;
    while (i < len) {
        char c = data[i];

        // --- Diverted content string: stream until the closing quote. ---
        if (js->divert) {
            if (c == '"') {
                js->divert = false;
                i++;
                continue;
            }
            if (c == '\\')
                return js->err = JSTREAM_ECONTENT;
            // Batch: find the end of this clean run.
            size_t start = i;
            while (i < len && data[i] != '"' && data[i] != '\\')
                i++;
            if (js->sink && js->sink(data + start, i - start, js->sink_ctx) != 0)
                return js->err = JSTREAM_ESINK;
            continue;
        }

        // --- Inside a normal (copied) string. ---
        if (js->in_string) {
            int rc = doc_append_ch(js, c);
            if (rc) return js->err = rc;
            if (js->esc) {
                js->esc = false;
                if (js->is_key)
                    js->key_overflow = true; // escaped keys can't match
            } else if (c == '\\') {
                js->esc = true;
            } else if (c == '"') {
                js->in_string = false;
                if (js->is_key)
                    key_complete(js);
            } else if (js->is_key) {
                if (js->key_len < JSTREAM_MAX_KEY)
                    js->key[js->key_len++] = c;
                else
                    js->key_overflow = true;
            }
            i++;
            continue;
        }

        // --- Structural / outside strings. ---
        int rc = 0;
        switch (c) {
            case '"': {
                bool starting_key = js->depth >= 1 &&
                                    js->is_obj[js->depth] && js->next_is_key;
                if (!starting_key && js->next_value_is_content) {
                    js->next_value_is_content = false;
                    js->next_value_on_path = false;
                    if (js->content_found)
                        return js->err = JSTREAM_EDUP;
                    js->content_found = true;
                    js->divert = true;
                    rc = doc_append(js, kPlaceholder, sizeof(kPlaceholder) - 1);
                } else {
                    js->in_string = true;
                    js->is_key = starting_key;
                    js->key_len = 0;
                    js->key_overflow = false;
                    if (!starting_key) {
                        js->next_value_is_content = false;
                        js->next_value_on_path = false;
                    }
                    rc = doc_append_ch(js, c);
                }
                break;
            }
            case '{':
            case '[': {
                if (js->depth >= JSTREAM_MAX_DEPTH)
                    return js->err = JSTREAM_EDEPTH;
                js->depth++;
                js->is_obj[js->depth] = (c == '{');
                js->on_path[js->depth] =
                    (js->depth == 1) ? true : js->next_value_on_path;
                // Arrays are never on the target path.
                if (c == '[')
                    js->on_path[js->depth] = false;
                js->next_value_on_path = false;
                js->next_value_is_content = false;
                js->next_is_key = (c == '{');
                rc = doc_append_ch(js, c);
                break;
            }
            case '}':
            case ']':
                if (js->depth <= 0)
                    return js->err = JSTREAM_EDEPTH;
                js->depth--;
                rc = doc_append_ch(js, c);
                break;
            case ':':
                js->next_is_key = false;
                rc = doc_append_ch(js, c);
                break;
            case ',':
                js->next_is_key = js->depth >= 1 && js->is_obj[js->depth];
                rc = doc_append_ch(js, c);
                break;
            case ' ': case '\t': case '\r': case '\n':
                rc = doc_append_ch(js, c);
                break;
            default:
                // Primitive value (number/true/false/null) consumes any
                // pending value match.
                js->next_value_on_path = false;
                js->next_value_is_content = false;
                rc = doc_append_ch(js, c);
                break;
        }
        if (rc)
            return js->err = rc;
        i++;
    }
    return 0;
}

int jstream_finish(Jstream *js, bool *content_found) {
    if (js->err)
        return js->err;
    if (js->in_string || js->divert || js->depth != 0)
        return js->err = JSTREAM_EPARTIAL;
    if (content_found)
        *content_found = js->content_found;
    return 0;
}
