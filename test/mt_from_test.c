#include "mt_from.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

/** Проверить production builder на корректном sender identity и безопасном user-part encoding. */
static void test_valid_sender_identity(void)
{
    char uri[256];
    assert(mt_from_uri_build("MegaFon@", "sipsis.keblog.ru", uri, sizeof(uri)) == 0);
    assert(strcmp(uri, "sip:MegaFon%40@sipsis.keblog.ru") == 0);
    assert(mt_from_uri_build("79958528857", "sipsis.keblog.ru", uri, sizeof(uri)) == 0);
    assert(strcmp(uri, "sip:79958528857@sipsis.keblog.ru") == 0);
    assert(mt_from_uri_build("MegaFon-1", "sipsis.keblog.ru", uri, sizeof(uri)) == 0);
    assert(strcmp(uri, "sip:MegaFon-1@sipsis.keblog.ru") == 0);
    assert(mt_from_uri_build("a;b/c?d=e&f", "sipsis.keblog.ru", uri, sizeof(uri)) == 0);
    assert(strcmp(uri, "sip:a%3Bb%2Fc%3Fd%3De%26f@sipsis.keblog.ru") == 0);
    /* Процентный знак исходных данных кодируется как %25, не интерпретируется как готовый escape. */
    assert(mt_from_uri_build("100%ready", "sipsis.keblog.ru", uri, sizeof(uri)) == 0);
    assert(strcmp(uri, "sip:100%25ready@sipsis.keblog.ru") == 0);
    /* В user-part может быть только кодированный @; ровно один сырой @ разделяет host. */
    const char* separator = strrchr(uri, '@');
    assert(separator && separator == strchr(uri, '@'));
    assert(strcmp(separator + 1, "sipsis.keblog.ru") == 0);
    assert(strstr(uri, "%40") == NULL); /* данный sender не содержит @ */
    assert(mt_from_uri_build("MegaFon@", "sipsis.keblog.ru", uri, sizeof(uri)) == 0);
    separator = strchr(uri, '@');
    assert(separator && separator == strrchr(uri, '@'));
    assert(strstr(uri, "MegaFon%40") == uri + 4);
    assert(strcmp(separator + 1, "sipsis.keblog.ru") == 0);
    /* Корректная UTF-8 последовательность кодируется байт-в-байт в URI user-part. */
    assert(mt_from_uri_build("caf\xC3\xA9", "sipsis.keblog.ru", uri, sizeof(uri)) == 0);
    assert(strcmp(uri, "sip:caf%C3%A9@sipsis.keblog.ru") == 0);
    assert(mt_from_uri_build("sender", "sipsis.keblog.ru:5061", uri, sizeof(uri)) == 0);
    assert(strcmp(uri, "sip:sender@sipsis.keblog.ru:5061") == 0);
    assert(mt_from_uri_build("sender", "[2001:db8::1]:5061", uri, sizeof(uri)) == 0);
    assert(strcmp(uri, "sip:sender@[2001:db8::1]:5061") == 0);
}

/** Некорректный/пустой identity и authority injection должны отклоняться без частичного URI. */
static void test_invalid_input_rejected(void)
{
    char uri[256] = "stale";
    assert(mt_from_uri_build("", "sipsis.keblog.ru", uri, sizeof(uri)) == -1 && uri[0] == '\0');
    assert(mt_from_uri_build(NULL, "sipsis.keblog.ru", uri, sizeof(uri)) == -1 && uri[0] == '\0');
    assert(mt_from_uri_build("MegaFon\r\nInjected: yes", "sipsis.keblog.ru", uri, sizeof(uri)) == -1 && uri[0] == '\0');
    assert(mt_from_uri_build("name\x01", "sipsis.keblog.ru", uri, sizeof(uri)) == -1 && uri[0] == '\0');
    assert(mt_from_uri_build("bad\xC3", "sipsis.keblog.ru", uri, sizeof(uri)) == -1 && uri[0] == '\0');
    assert(mt_from_uri_build("bad\xC0\xAF", "sipsis.keblog.ru", uri, sizeof(uri)) == -1 && uri[0] == '\0');
    assert(mt_from_uri_build("bad\x80", "sipsis.keblog.ru", uri, sizeof(uri)) == -1 && uri[0] == '\0');
    assert(mt_from_uri_build("sender", "", uri, sizeof(uri)) == -1 && uri[0] == '\0');
    assert(mt_from_uri_build("sender", "host\r\nInjected", uri, sizeof(uri)) == -1 && uri[0] == '\0');
    assert(mt_from_uri_build("sender", "host@evil", uri, sizeof(uri)) == -1 && uri[0] == '\0');
    assert(mt_from_uri_build("sender", "bad..host", uri, sizeof(uri)) == -1 && uri[0] == '\0');
    assert(mt_from_uri_build("sender", "host:bad", uri, sizeof(uri)) == -1 && uri[0] == '\0');
    assert(mt_from_uri_build("sender", "[not-ipv6]", uri, sizeof(uri)) == -1 && uri[0] == '\0');
    assert(mt_from_uri_build("long-sender", "sipsis.keblog.ru", uri, 8) == -1 && uri[0] == '\0');
    char exact[11];
    assert(mt_from_uri_build("x", "host", exact, sizeof(exact)) == 0);
    assert(strcmp(exact, "sip:x@host") == 0);
    assert(mt_from_uri_build("x", "host", exact, sizeof(exact) - 1) == -1 && exact[0] == '\0');
    assert(mt_from_uri_build("sender", "sipsis.keblog.ru", NULL, 10) == -1);
}

int main(void)
{
    test_valid_sender_identity();
    test_invalid_input_rejected();
    puts("mt-from: all checks passed");
    return 0;
}
