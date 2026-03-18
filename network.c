// network.c
// Implementacja klienta HTTP opartego na libcurl.
// Obsługuje zapytania GET z autoryzacją API i parsowanie nagłówka Retry-After.

#define _DEFAULT_SOURCE

#include "network.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Bufor pomocniczy do akumulacji odpowiedzi HTTP
typedef struct {
    char *data;
    size_t size;
} Buf;

// Callback zapisu danych odpowiedzi (wywoływany przez libcurl)
static size_t write_cb(void *ptr, size_t size, size_t nmemb, void *userdata) {
    size_t n = size * nmemb;
    Buf *b = (Buf *)userdata;
    char *p = (char *)realloc(b->data, b->size + n + 1);
    if (!p) return 0;
    b->data = p;
    memcpy(b->data + b->size, ptr, n);
    b->size += n;
    b->data[b->size] = '\0';
    return n;
}

// Stan parsowania nagłówków (wyciąga Retry-After)
typedef struct {
    long retry_after_s;
} HeaderState;

// Callback parsowania nagłówków odpowiedzi HTTP
static size_t header_cb(char *buffer, size_t size, size_t nitems, void *userdata) {
    size_t n = size * nitems;
    HeaderState *hs = (HeaderState *)userdata;
    const char *k = "Retry-After:";
    size_t klen = strlen(k);
    if (n >= klen && strncasecmp(buffer, k, klen) == 0) {
        const char *p = buffer + klen;
        while ((p < buffer + n) && (*p == ' ' || *p == '\t')) p++;
        long v = 0;
        while (p < buffer + n && isdigit((unsigned char)*p)) {
            v = v * 10 + (*p - '0');
            p++;
        }
        if (v > 0) hs->retry_after_s = v;
    }
    return n;
}

HttpResp http_get(const char *url, const char *api_secret) {
    HttpResp r = {0};
    r.retry_after_s = -1;

    CURL *c = curl_easy_init();
    if (!c) {
        r.curl_rc = CURLE_FAILED_INIT;
        return r;
    }

    Buf b = {0};
    b.data = (char *)malloc(1);
    if (!b.data) {
        curl_easy_cleanup(c);
        r.curl_rc = CURLE_OUT_OF_MEMORY;
        return r;
    }
    b.data[0] = '\0';

    HeaderState hs = {.retry_after_s = -1};

    struct curl_slist *hdr = NULL;
    hdr = curl_slist_append(hdr, "accept: application/json");
    char secret_hdr[512];
    snprintf(secret_hdr, sizeof(secret_hdr), "secret: %s", api_secret);
    hdr = curl_slist_append(hdr, secret_hdr);

    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdr);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &b);
    curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, header_cb);
    curl_easy_setopt(c, CURLOPT_HEADERDATA, &hs);
    curl_easy_setopt(c, CURLOPT_USERAGENT, "lotto/2.0");
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 25L);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, "");

    CURLcode rc = curl_easy_perform(c);
    r.curl_rc = rc;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &r.http_code);
    r.retry_after_s = hs.retry_after_s;

    curl_slist_free_all(hdr);
    curl_easy_cleanup(c);

    if (rc != CURLE_OK) {
        free(b.data);
        r.body = NULL;
    } else {
        r.body = b.data;
    }
    return r;
}

// Wycinki odpowiedzi do logowania błędów (maks. 300 znaków)
void report_fetch_error(const char *context, CURLcode curl_rc, long http_code, const char *body) {
    if (curl_rc != CURLE_OK) {
        fprintf(stderr, "%s: CURL error: %s\n", context, curl_easy_strerror(curl_rc));
        return;
    }

    fprintf(stderr, "%s: HTTP %ld\n", context, http_code);
    if (body && *body) {
        char snippet[301];
        snprintf(snippet, sizeof(snippet), "%.300s", body);
        fprintf(stderr, "%s: response body: %s\n", context, snippet);
    }
}
