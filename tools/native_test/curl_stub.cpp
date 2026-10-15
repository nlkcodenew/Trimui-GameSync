// curl_stub.cpp — stub bodies for native unit tests (never called).
#include "curl/curl.h"

#include <cstdarg>
#include <cstdlib>
#include <cstring>

extern "C" {

CURL *curl_easy_init(void) { return nullptr; }
void curl_easy_cleanup(CURL *) {}
CURLcode curl_easy_perform(CURL *) { return CURLE_OK; }
CURLcode curl_easy_setopt(CURL *, CURLoption, ...) { return CURLE_OK; }
CURLcode curl_easy_getinfo(CURL *, CURLINFO, ...) { return CURLE_OK; }
const char *curl_easy_strerror(CURLcode) { return "stub"; }
char *curl_easy_escape(CURL *, const char *s, int len) {
  if (!s) return nullptr;
  size_t n = (len < 0) ? strlen(s) : (size_t)len;
  char *o = (char *)malloc(n + 1);
  if (o) {
    memcpy(o, s, n);
    o[n] = '\0';
  }
  return o;
}
void curl_free(void *p) { free(p); }

}  // extern "C"
