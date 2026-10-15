// curl_stub.h — minimal curl declarations for native (host) unit tests.
// Only the symbols used by src/Drive.cpp are stubbed. Never performs I/O.
#pragma once

typedef void CURL;
typedef int CURLcode;
typedef int CURLoption;
typedef int CURLINFO;

#define CURLE_OK 0
#define CURLE_WRITE_ERROR 23
#define CURLE_ABORTED_BY_CALLBACK 42

#define CURLOPT_URL 10002
#define CURLOPT_WRITEFUNCTION 20011
#define CURLOPT_WRITEDATA 10001
#define CURLOPT_HEADERFUNCTION 20079
#define CURLOPT_HEADERDATA 10029
#define CURLOPT_RANGE 10007
#define CURLOPT_USERAGENT 10018
#define CURLOPT_CONNECTTIMEOUT 78
#define CURLOPT_TIMEOUT 13
#define CURLOPT_LOW_SPEED_LIMIT 19
#define CURLOPT_LOW_SPEED_TIME 20
#define CURLOPT_FOLLOWLOCATION 52
#define CURLOPT_MAXREDIRS 68
#define CURLOPT_CAINFO 10065
#define CURLOPT_SSL_VERIFYPEER 64
#define CURLOPT_ERRORBUFFER 10010

#define CURLINFO_RESPONSE_CODE 0x200002
#define CURL_ERROR_SIZE 256

#ifdef __cplusplus
extern "C" {
#endif

CURL *curl_easy_init(void);
void curl_easy_cleanup(CURL *h);
CURLcode curl_easy_perform(CURL *h);
CURLcode curl_easy_setopt(CURL *h, CURLoption opt, ...);
CURLcode curl_easy_getinfo(CURL *h, CURLINFO info, ...);
const char *curl_easy_strerror(CURLcode rc);
char *curl_easy_escape(CURL *h, const char *s, int len);
void curl_free(void *p);

#ifdef __cplusplus
}
#endif
