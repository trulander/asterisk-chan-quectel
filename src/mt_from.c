#include "mt_from.h"

#include <arpa/inet.h>
#include <stdint.h>
#include <string.h>

/** Проверить port suffix в host[:port] без принятия URI/header delimiters. */
static int valid_port(const char* port)
{
    if (!port || !*port) return 0;
    unsigned value = 0;
    for (const unsigned char* p = (const unsigned char*)port; *p; ++p) {
        if (*p < '0' || *p > '9') return 0;
        value = value * 10 + (*p - '0');
        if (value > 65535) return 0;
    }
    return value > 0;
}

/** Проверить DNS/IPv4 либо bracketed IPv6 authority для From URI host. */
static int valid_domain(const char* domain)
{
    if (!domain || !*domain) return 0;
    if (*domain == '[') {
        const char* close = strchr(domain + 1, ']');
        if (!close) return 0;
        const size_t n = (size_t)(close - domain - 1);
        if (!n || n >= INET6_ADDRSTRLEN) return 0;
        char address[INET6_ADDRSTRLEN];
        memcpy(address, domain + 1, n);
        address[n] = '\0';
        struct in6_addr parsed;
        if (inet_pton(AF_INET6, address, &parsed) != 1) return 0;
        return !close[1] || (close[1] == ':' && valid_port(close + 2));
    }

    const char* colon = strchr(domain, ':');
    const size_t host_len = colon ? (size_t)(colon - domain) : strlen(domain);
    if (!host_len || (colon && (!valid_port(colon + 1) || strchr(colon + 1, ':')))) return 0;
    if (domain[0] == '.' || domain[host_len - 1] == '.' || strstr(domain, "..")) return 0;
    size_t label_len = 0;
    for (size_t i = 0; i < host_len; ++i) {
        const unsigned char c = (unsigned char)domain[i];
        if (c == '.') {
            if (!label_len || domain[i - 1] == '-') return 0;
            label_len = 0;
        } else if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                   (c >= '0' && c <= '9') || c == '-') {
            if (!label_len && c == '-') return 0;
            ++label_len;
        } else return 0;
    }
    return label_len && domain[host_len - 1] != '-';
}

/** Отклонить обрезанный/некорректный UTF-8, не меняя корректный sender identity. */
static int valid_utf8(const unsigned char* s)
{
    const size_t length = strlen((const char*)s);
    size_t i = 0;
    while (i < length) {
        const unsigned char c = s[i];
        if (c < 0x80) { ++i; continue; }
        if (c >= 0xc2 && c <= 0xdf) {
            if (length - i < 2 || (s[i + 1] & 0xc0) != 0x80) return 0;
            i += 2;
        } else if (c >= 0xe0 && c <= 0xef) {
            if (length - i < 3 || (s[i + 1] & 0xc0) != 0x80 || (s[i + 2] & 0xc0) != 0x80 ||
                (c == 0xe0 && s[i + 1] < 0xa0) || (c == 0xed && s[i + 1] >= 0xa0)) return 0;
            i += 3;
        } else if (c >= 0xf0 && c <= 0xf4) {
            if (length - i < 4 || (s[i + 1] & 0xc0) != 0x80 ||
                (s[i + 2] & 0xc0) != 0x80 || (s[i + 3] & 0xc0) != 0x80 ||
                (c == 0xf0 && s[i + 1] < 0x90) || (c == 0xf4 && s[i + 1] >= 0x90)) return 0;
            i += 4;
        } else return 0;
    }
    return 1;
}

/** RFC 3261 unreserved URI characters остаются буквальными; прочие байты экранируются. */
static int uri_unreserved(unsigned char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_' ||
           c == '~' || c == '!' || c == '*' || c == '\'' || c == '(' || c == ')';
}

int mt_from_uri_build(const char* sender, const char* domain, char* out, size_t out_len)
{
    static const char hex[] = "0123456789ABCDEF";
    if (!out || !out_len) return -1;
    out[0] = '\0';
    if (!sender || !*sender || !valid_utf8((const unsigned char*)sender) || !valid_domain(domain)) return -1;

    const size_t domain_len = strlen(domain);
    if (domain_len > SIZE_MAX - 6 || out_len <= 4 + domain_len + 1) return -1;
    memcpy(out, "sip:", 4);
    size_t used = 4;
    for (const unsigned char* p = (const unsigned char*)sender; *p; ++p) {
        const unsigned char c = *p;
        if (c < 0x20 || c == 0x7f) { out[0] = '\0'; return -1; }
        const size_t need = uri_unreserved(c) ? 1 : 3;
        if (used > out_len - domain_len - 2 || need > out_len - domain_len - 2 - used) {
            out[0] = '\0';
            return -1;
        }
        if (need == 1) out[used++] = (char)c;
        else {
            out[used++] = '%';
            out[used++] = hex[c >> 4];
            out[used++] = hex[c & 0x0f];
        }
    }
    if (used == 4 || used + 1 + domain_len + 1 > out_len) { out[0] = '\0'; return -1; }
    out[used++] = '@';
    memcpy(out + used, domain, domain_len);
    out[used + domain_len] = '\0';
    return 0;
}
