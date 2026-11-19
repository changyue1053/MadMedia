/* Dependency link proof only. This source never opens a socket or transfers data. */
#include <curl/curl.h>
#include <mbedtls/build_info.h>
#include <mbedtls/ssl.h>

#if !defined(__ANDROID__) || !defined(__aarch64__) || __ANDROID_API__ != 28
#error Android ARM64 API 28 required
#endif
_Static_assert(LIBCURL_VERSION_NUM == 0x081700, "curl 8.23.0 required");
_Static_assert(MBEDTLS_VERSION_NUMBER == 0x04010200, "Mbed TLS 4.1.2 required");

int main(void) {
    mbedtls_ssl_context ssl;
    mbedtls_ssl_init(&ssl);
    mbedtls_ssl_free(&ssl);
    CURL *easy = curl_easy_init();
    CURLM *multi = curl_multi_init();
    CURLU *url = curl_url();
    int failed = !easy || !multi || !url;
    if (easy) {
        failed |= curl_easy_setopt(easy,CURLOPT_SSL_VERIFYPEER,1L)!=CURLE_OK;
        failed |= curl_easy_setopt(easy,CURLOPT_SSL_VERIFYHOST,2L)!=CURLE_OK;
        failed |= curl_easy_setopt(easy,CURLOPT_FOLLOWLOCATION,0L)!=CURLE_OK;
        failed |= curl_easy_setopt(easy,CURLOPT_PROTOCOLS_STR,"https")!=CURLE_OK;
        failed |= curl_easy_setopt(easy,CURLOPT_CAINFO_BLOB,(struct curl_blob *)0)!=CURLE_OK;
        curl_easy_cleanup(easy);
    }
    if (multi) curl_multi_cleanup(multi);
    if (url) curl_url_cleanup(url);
    return failed;
}
