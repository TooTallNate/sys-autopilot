#include "jsmn.h" // implementation (no JSMN_HEADER)
#include "json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int json_parse(JsonDoc *doc, const char *src, size_t len) {
    jsmn_parser p;
    jsmn_init(&p);
    doc->src = src;
    int n = jsmn_parse(&p, src, len, doc->tok, JSON_MAX_TOKENS);
    if (n < 0)
        return n;
    doc->ntok = n;
    return 0;
}

static int tok_len(const JsonDoc *doc, int tok) {
    return doc->tok[tok].end - doc->tok[tok].start;
}

static const char *tok_ptr(const JsonDoc *doc, int tok) {
    return doc->src + doc->tok[tok].start;
}

bool json_streq(const JsonDoc *doc, int tok, const char *s) {
    int len = tok_len(doc, tok);
    return (int)strlen(s) == len && memcmp(tok_ptr(doc, tok), s, (size_t)len) == 0;
}

int json_skip(const JsonDoc *doc, int tok) {
    int end = doc->tok[tok].end;
    int i = tok + 1;
    while (i < doc->ntok && doc->tok[i].start < end)
        i++;
    return i;
}

int json_obj_get(const JsonDoc *doc, int obj, const char *key) {
    if (obj < 0 || obj >= doc->ntok || doc->tok[obj].type != JSMN_OBJECT)
        return -1;
    int i = obj + 1;
    for (int n = 0; n < doc->tok[obj].size; n++) {
        int val = i + 1;
        if (val >= doc->ntok)
            return -1;
        if (doc->tok[i].type == JSMN_STRING && json_streq(doc, i, key))
            return val;
        i = json_skip(doc, val);
    }
    return -1;
}

int json_arr_len(const JsonDoc *doc, int arr) {
    if (arr < 0 || arr >= doc->ntok || doc->tok[arr].type != JSMN_ARRAY)
        return -1;
    return doc->tok[arr].size;
}

int json_arr_get(const JsonDoc *doc, int arr, int idx) {
    if (json_arr_len(doc, arr) <= idx || idx < 0)
        return -1;
    int i = arr + 1;
    for (int n = 0; n < idx; n++)
        i = json_skip(doc, i);
    return i;
}

bool json_get_string(const JsonDoc *doc, int tok, char *out, size_t outsz) {
    if (tok < 0 || tok >= doc->ntok || doc->tok[tok].type != JSMN_STRING)
        return false;
    const char *p = tok_ptr(doc, tok);
    int len = tok_len(doc, tok);
    size_t o = 0;
    for (int i = 0; i < len; i++) {
        if (o + 4 >= outsz)
            return false;
        char c = p[i];
        if (c != '\\') {
            out[o++] = c;
            continue;
        }
        if (++i >= len)
            return false;
        switch (p[i]) {
            case '"':  out[o++] = '"';  break;
            case '\\': out[o++] = '\\'; break;
            case '/':  out[o++] = '/';  break;
            case 'n':  out[o++] = '\n'; break;
            case 't':  out[o++] = '\t'; break;
            case 'r':  out[o++] = '\r'; break;
            case 'b':  out[o++] = '\b'; break;
            case 'f':  out[o++] = '\f'; break;
            case 'u': {
                if (i + 4 >= len)
                    return false;
                char hex[5] = { p[i+1], p[i+2], p[i+3], p[i+4], 0 };
                unsigned cp = (unsigned)strtoul(hex, NULL, 16);
                i += 4;
                // Encode BMP codepoint as UTF-8 (surrogate pairs unsupported;
                // emitted as '?').
                if (cp < 0x80) {
                    out[o++] = (char)cp;
                } else if (cp < 0x800) {
                    out[o++] = (char)(0xC0 | (cp >> 6));
                    out[o++] = (char)(0x80 | (cp & 0x3F));
                } else if (cp >= 0xD800 && cp <= 0xDFFF) {
                    out[o++] = '?';
                } else {
                    out[o++] = (char)(0xE0 | (cp >> 12));
                    out[o++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                    out[o++] = (char)(0x80 | (cp & 0x3F));
                }
                break;
            }
            default:
                return false;
        }
    }
    out[o] = '\0';
    return true;
}

bool json_get_int(const JsonDoc *doc, int tok, long long *out) {
    if (tok < 0 || tok >= doc->ntok || doc->tok[tok].type != JSMN_PRIMITIVE)
        return false;
    char buf[32];
    if (!json_raw(doc, tok, buf, sizeof(buf)))
        return false;
    char *end = NULL;
    long long v = strtoll(buf, &end, 10);
    if (end == buf)
        return false;
    *out = v;
    return true;
}

bool json_get_double(const JsonDoc *doc, int tok, double *out) {
    if (tok < 0 || tok >= doc->ntok || doc->tok[tok].type != JSMN_PRIMITIVE)
        return false;
    char buf[48];
    if (!json_raw(doc, tok, buf, sizeof(buf)))
        return false;
    char *end = NULL;
    double v = strtod(buf, &end);
    if (end == buf)
        return false;
    *out = v;
    return true;
}

bool json_get_bool(const JsonDoc *doc, int tok, bool *out) {
    if (tok < 0 || tok >= doc->ntok || doc->tok[tok].type != JSMN_PRIMITIVE)
        return false;
    if (json_streq(doc, tok, "true"))  { *out = true;  return true; }
    if (json_streq(doc, tok, "false")) { *out = false; return true; }
    return false;
}

bool json_raw(const JsonDoc *doc, int tok, char *out, size_t outsz) {
    if (tok < 0 || tok >= doc->ntok)
        return false;
    int start = doc->tok[tok].start;
    int end = doc->tok[tok].end;
    if (doc->tok[tok].type == JSMN_STRING) {
        start -= 1; // include quotes
        end += 1;
    }
    size_t len = (size_t)(end - start);
    if (len + 1 > outsz)
        return false;
    memcpy(out, doc->src + start, len);
    out[len] = '\0';
    return true;
}

static size_t escape_char(char c, char *out /* >= 6 bytes or NULL */) {
    const char *rep = NULL;
    switch (c) {
        case '"':  rep = "\\\""; break;
        case '\\': rep = "\\\\"; break;
        case '\n': rep = "\\n";  break;
        case '\t': rep = "\\t";  break;
        case '\r': rep = "\\r";  break;
        case '\b': rep = "\\b";  break;
        case '\f': rep = "\\f";  break;
        default:
            if ((unsigned char)c < 0x20) {
                if (out)
                    snprintf(out, 7, "\\u%04x", (unsigned char)c);
                return 6;
            }
            if (out)
                *out = c;
            return 1;
    }
    if (out)
        memcpy(out, rep, 2);
    return 2;
}

size_t json_escaped_len(const char *in, size_t inlen) {
    size_t n = 0;
    for (size_t i = 0; i < inlen; i++)
        n += escape_char(in[i], NULL);
    return n;
}

size_t json_escape(const char *in, size_t inlen, char *out, size_t outsz) {
    size_t o = 0;
    for (size_t i = 0; i < inlen; i++) {
        char tmp[8];
        size_t n = escape_char(in[i], tmp);
        if (o + n + 1 > outsz)
            return (size_t)-1;
        memcpy(out + o, tmp, n);
        o += n;
    }
    out[o] = '\0';
    return o;
}
