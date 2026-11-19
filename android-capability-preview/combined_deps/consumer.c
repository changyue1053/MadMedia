/* Independent Android link evidence only. No network or file access. */
#include <curl/curl.h>
#include <mbedtls/build_info.h>
#include <mbedtls/ssl.h>
#include <sqlite3.h>

#if !defined(__ANDROID__) || !defined(__aarch64__) || __ANDROID_API__ != 28
#error Android ARM64 API 28 required
#endif
_Static_assert(LIBCURL_VERSION_NUM==0x081700, "curl 8.23.0 required");
_Static_assert(MBEDTLS_VERSION_NUMBER==0x04010200, "Mbed TLS 4.1.2 required");
_Static_assert(SQLITE_VERSION_NUMBER==3054001, "SQLite 3.54.1 required");

int main(void) {
    CURL *easy=curl_easy_init();
    CURLM *multi=curl_multi_init();
    CURLU *url=curl_url();
    int failure=!easy || !multi || !url;
    if (easy) {
        failure |= curl_easy_setopt(easy,CURLOPT_SSL_VERIFYPEER,1L)!=CURLE_OK;
        failure |= curl_easy_setopt(easy,CURLOPT_SSL_VERIFYHOST,2L)!=CURLE_OK;
        curl_easy_cleanup(easy);
    }
    if (multi) curl_multi_cleanup(multi);
    if (url) curl_url_cleanup(url);
    mbedtls_ssl_context ssl;
    mbedtls_ssl_init(&ssl);
    mbedtls_ssl_free(&ssl);
    sqlite3 *db=0;
    sqlite3_stmt *stmt=0;
    failure |= !sqlite3_sourceid();
    if (sqlite3_open_v2(":memory:",&db,SQLITE_OPEN_READWRITE|SQLITE_OPEN_CREATE,0)==SQLITE_OK) {
        failure |= sqlite3_prepare_v2(db,"SELECT 1",-1,&stmt,0)!=SQLITE_OK;
        failure |= sqlite3_wal_checkpoint_v2(db,0,SQLITE_CHECKPOINT_PASSIVE,0,0)!=SQLITE_OK;
        if (stmt) sqlite3_finalize(stmt);
    } else failure=1;
    if (db) sqlite3_close(db);
    return failure!=0;
}
