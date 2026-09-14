#include "qpack.h"
#include "huffman.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* ── QPACK static table name lookup (RFC 9204 §A, indices 0-98) ───── */
static const char *st_name[99] = {
    [ 0]=":authority", [ 1]=":path",       [ 2]="age",
    [ 3]="content-disposition",            [ 4]="content-length",
    [ 5]="cookie",     [ 6]="date",        [ 7]="etag",
    [ 8]="if-modified-since",              [ 9]="if-none-match",
    [10]="last-modified",[11]="link",      [12]="location",
    [13]="referer",    [14]="set-cookie",  [15]="strict-transport-security",
    [16]="transfer-encoding",              [17]="user-agent",
    [18]="vary",       [19]="cache-control",[20]="content-encoding",
    [21]="content-language",               [22]="content-location",
    [23]="content-range",[24]=":method",   [25]=":scheme",
    [26]=":status",    [27]=":authority",  [28]="accept",
    [29]="accept-encoding",                [30]="accept-language",
    [31]="alt-svc",    [32]="dns",         [33]="expect",
    [34]="forwarded",  [35]="from",        [36]="host",
    [37]="max-forwards",[38]="origin",     [39]="proxy-authenticate",
    [40]="proxy-authorization",            [41]="range",
    [42]="te",         [43]="trailer",     [44]="upgrade",
    [45]="user-agent", [46]="via",         [47]="x-forwarded-for",
    [48]="x-forwarded-host",               [49]="x-forwarded-proto",
    [50]="x-request-id",[51]="cache-control",[52]="content-type",
    [53]="date",       [54]="expect",      [55]="forwarded",
    [56]="if-match",   [57]="if-modified-since",[58]="if-none-match",
    [59]="if-range",   [60]="if-unmodified-since",[61]="location",
    [62]="server",     [63]="strict-transport-security",
    [64]="transfer-encoding",              [65]="user-agent",
    [66]="vary",       [67]="content-security-policy",
    [68]="x-content-type-options",         [69]="x-frame-options",
    [70]="x-xss-protection",[71]="accept-ranges",[72]="cache-control",
    [73]="content-length",[74]="content-type",
    [75]="cross-origin-resource-policy",
    [76]="expect-ct",  [77]="nel",         [78]="report-to",
    [79]="server-timing",[80]="timing-allow-origin",
    [81]="upgrade-insecure-requests",      [82]="x-content-type-options",
    [83]="x-frame-options",[84]="x-xss-protection",
    [85]="accept-ranges",[86]="access-control-allow-origin",
    [87]="access-control-allow-credentials",[88]="access-control-max-age",
    [89]="alt-svc",    [90]="via",         [91]="x-forwarded-for",
    [92]="x-forwarded-host",[93]="x-forwarded-proto",[94]="x-request-id",
    [95]="cache-control",[96]="content-type",[97]="date",
    [98]="expect",
};

/* ── QPACK static table values (RFC 9204 §A) ───── */
/* Only entries that have both name AND value in the static table */
static const char *st_value[99] = {
    [ 1] = "/",            /* :path / */
    [ 4] = "0",            /* content-length 0 */
    [ 6] = "0",            /* date 0 */
    [24] = "GET",          /* :method GET */
    [25] = "POST",         /* :method POST */
    [26] = "http",         /* :scheme http */
    [27] = "https",        /* :scheme https */
};

const char* qpack_static_name(uint64_t index) {
    return (index < 99) ? st_name[index] : NULL;
}

const char* qpack_static_value(uint64_t index) {
    return (index < 99) ? st_value[index] : NULL;
}

/* ── Varint decode ────────────────────────────────────────── */
static int vd(const uint8_t *buf, size_t len, int prefix, uint64_t *val) {
    if (len < 1) return -1;
    uint64_t mask = (1ULL << prefix) - 1;
    *val = buf[0] & mask;
    if (*val < mask) return 1;
    size_t pos = 1;
    uint64_t m = 0;
    while (pos < len && pos < 10) {
        uint8_t b = buf[pos++];
        *val += (uint64_t)(b & 0x7f) << m;
        if (!(b & 0x80)) return (int)pos;
        m += 7;
    }
    return -1;
}

/* ── Huffman decode helper ────────────────────────────────── */
/* Using nghttp3-verified state-machine decoder */
static int huff_str(uint8_t *dst, size_t cap,
                     const uint8_t *src, size_t len) {
    huffman_decode_ctx ctx;
    huffman_decode_init(&ctx);
    int r = huffman_decode_str(&ctx, dst, src, len, 1 /* fin */);
    if (r > 0 && (size_t)r < cap) dst[r] = '\0';
    return r;
}

/* ── Parse single header field ───────────────────────────── */
static int parse_one(const uint8_t *data, size_t len, QpackHeaderField *f) {
    if (len < 1) return -1;
    uint8_t b = data[0];

    /* Indexed Header Field: 1 T Index(6+) */
    if (b & 0x80) {
        uint64_t idx;
        int v = vd(data, len, 6, &idx);
        if (v < 0) return -1;
        f->name = f->value = NULL; f->name_len = f->value_len = 0;
        if ((b>>6)&1) {
            const char *n = qpack_static_name(idx);
            if (n) { f->name=(const uint8_t*)n; f->name_len=strlen(n); }
            const char *val = qpack_static_value(idx);
            if (val) { f->value=(const uint8_t*)val; f->value_len=strlen(val); }
        }
        return v;
    }

    /* Literal Header Field With Name Reference: 01 N T NameIndex(4+) */
    if ((b & 0xc0) == 0x40) {
        uint64_t idx; int v = vd(data, len, 4, &idx);
        if (v < 0) return -1;
        int t = (b>>4)&1;
        if (!t || (size_t)v >= len) return -1;

        uint64_t vlen; int v2 = vd(data+v, len-v, 7, &vlen);
        if (v2 < 0) return -1;
        int h = (data[v]>>7)&1;
        size_t p = (size_t)(v+v2);
        if (p+(size_t)vlen > len) return -1;

        f->name = f->value = NULL; f->name_len = f->value_len = 0;
        const char *nm = qpack_static_name(idx);
        if (nm) { f->name=(const uint8_t*)nm; f->name_len=strlen(nm); }

        if (!h) {
            f->value = data+p; f->value_len = (size_t)vlen;
        } else {
            static uint8_t hb[16][4096]; static int slot;
            uint8_t *buf = hb[slot]; slot = (slot+1)&15;
            int dl = huff_str(buf, 4095, data+p, (size_t)vlen);
            f->value    = (dl>0) ? buf : NULL;
            f->value_len= (dl>0) ? (size_t)dl : 0;
        }
        return v + v2 + (int)vlen;
    }

    /* ── Literal Header Field With Literal Name ──
     * 001 | N | H | NameLength(3+) */
    if ((b & 0xe0) == 0x20) {
        int h_name = (b >> 3) & 1;  /* Huffman encoded name */
        uint64_t nlen; int v = vd(data, len, 3, &nlen);
        if (v < 0 || (size_t)v + (size_t)nlen > len) return -1;

        static uint8_t name_ring[8][256]; static int ns = 0;
        uint8_t *name_buf = name_ring[ns]; ns = (ns+1)&7;
        const uint8_t *name_ptr;
        int name_len;
        if (!h_name) {
            name_ptr = data + v; name_len = (int)nlen;
        } else {
            name_len = huff_str(name_buf, 255, data+v, (size_t)nlen);
            name_buf[name_len] = '\0'; name_ptr = name_buf;
        }
        size_t pos = (size_t)v + (size_t)nlen;
        if (pos >= len) return -1;

        uint64_t vlen; int h_val = (data[pos] >> 7) & 1;
        int v2 = vd(data + pos, len - pos, 7, &vlen);
        if (v2 < 0) return -1;
        pos += (size_t)v2;
        if (pos + (size_t)vlen > len) return -1;

        f->name     = name_ptr; f->name_len = (size_t)name_len;
        if (!h_val) {
            f->value = data + pos; f->value_len = (size_t)vlen;
        } else {
            static uint8_t hb[16][4096]; static int slot;
            uint8_t *buf = hb[slot]; slot = (slot+1)&15;
            int dl = huff_str(buf, 4095, data+pos, (size_t)vlen);
            f->value = (dl>0) ? buf : NULL; f->value_len = (dl>0) ? (size_t)dl : 0;
        }
        return (int)pos + (int)vlen;
    }

    return -1;
}

/* ── Public API ──────────────────────────────────────────── */
int qpack_decode(const uint8_t *data, size_t len,
                 QpackHeaderField *fields, int max_fields) {
    if (len < 2) return -1;
    size_t pos = 0; uint64_t d; int r;
    r = vd(data, len, 8, &d);   if (r<0) return -1; pos += r; (void)d;
    r = vd(data+pos, len-pos,7,&d); if (r<0) return -1; pos += r; (void)d;

    int count = 0;
    while (pos < len && count < max_fields) {
        int c = parse_one(data+pos, len-pos, &fields[count]);
        if (c > 0) { pos+=c; count++; } else break;
    }
    return count;
}
