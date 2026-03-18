// network.h
// Obsługa połączeń HTTP (libcurl): zapytania GET do API Lotto.

#ifndef NETWORK_H
#define NETWORK_H

#include <curl/curl.h>

// Wynik zapytania HTTP
typedef struct {
    char *body;           // treść odpowiedzi (alokowana dynamicznie, zwolnić przez free())
    long http_code;       // kod odpowiedzi HTTP (200, 429, 404...)
    long retry_after_s;   // wartość nagłówka Retry-After w sekundach (-1 jeśli brak)
    CURLcode curl_rc;     // kod powrotu libcurl
} HttpResp;

// Wykonuje zapytanie GET z nagłówkiem autoryzacji API
HttpResp http_get(const char *url, const char *api_secret);

// Raportuje błąd połączenia na stderr
void report_fetch_error(const char *context, CURLcode curl_rc, long http_code, const char *body);

#endif
