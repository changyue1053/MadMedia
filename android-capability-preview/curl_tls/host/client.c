/* Linux host fixture only. It can only call the names mapped to loopback. */
#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

_Static_assert(LIBCURL_VERSION_NUM == 0x081700, "curl 8.23.0 required");
static const char payload[] = "madmedia synthetic loopback\n";

struct response { char bytes[sizeof(payload)]; size_t used; };
static size_t collect(char *data, size_t size, size_t count, void *context) {
    struct response *out = context;
    if (size && count > (sizeof(out->bytes)-out->used)/size) return 0;
    size_t length = size*count;
    memcpy(out->bytes+out->used,data,length);
    out->used+=length;
    return length;
}

int main(int argc, char **argv) {
    int bad_name=0, reject=0, result=1;
    if (argc!=4) return 1;
    if (strcmp(argv[3],"wrong-name")==0) { bad_name=1; reject=1; }
    else if (strcmp(argv[3],"wrong-ca")==0) reject=1;
    else if (strcmp(argv[3],"match")!=0) return 1;
    char *end=NULL;
    long port=strtol(argv[1],&end,10);
    if (!end || *end || port<1024 || port>65535) return 1;

    unsigned char ca[8192];
    FILE *file=fopen(argv[2],"rb");
    if (!file) return 1;
    size_t length=fread(ca,1,sizeof(ca),file);
    int valid=length>0 && length<sizeof(ca) && !ferror(file) && feof(file);
    if (fclose(file)!=0 || !valid) return 1;
    struct curl_blob trust={ca,length,CURL_BLOB_COPY};

    if (curl_global_init(CURL_GLOBAL_DEFAULT)!=CURLE_OK) return 1;
    CURL *easy=NULL;
    struct curl_slist *resolve=NULL;
    curl_version_info_data *version=curl_version_info(CURLVERSION_NOW);
    if (!version || !version->version || strcmp(version->version,"8.23.0")!=0 ||
        !version->ssl_version || strcmp(version->ssl_version,"mbedTLS/4.1.2")!=0 ||
        !(version->features & CURL_VERSION_SSL) || (version->features & CURL_VERSION_MULTI_SSL)) goto done;
    const char *name=bad_name?"wrong.test":"media.test";
    char url[96], mapping[96];
    int url_size=snprintf(url,sizeof(url),"https://%s:%ld/madmedia-proof",name,port);
    int map_size=snprintf(mapping,sizeof(mapping),"%s:%ld:127.0.0.1",name,port);
    if (url_size<0 || (size_t)url_size>=sizeof(url) ||
        map_size<0 || (size_t)map_size>=sizeof(mapping)) goto done;
    resolve=curl_slist_append(NULL,mapping);
    easy=curl_easy_init();
    if (!resolve || !easy) goto done;

    struct response body={{0},0};
#define SET(key,value) do { if(curl_easy_setopt(easy,(key),(value))!=CURLE_OK) goto done; } while (0)
    SET(CURLOPT_URL,url);
    SET(CURLOPT_RESOLVE,resolve);
    SET(CURLOPT_PROTOCOLS_STR,"https");
    SET(CURLOPT_HTTP_VERSION,(long)CURL_HTTP_VERSION_1_1);
    SET(CURLOPT_IPRESOLVE,(long)CURL_IPRESOLVE_V4);
    SET(CURLOPT_FOLLOWLOCATION,0L);
    SET(CURLOPT_MAXREDIRS,0L);
    SET(CURLOPT_SSL_VERIFYPEER,1L);
    SET(CURLOPT_SSL_VERIFYHOST,2L);
    SET(CURLOPT_CAINFO_BLOB,&trust);
    SET(CURLOPT_SSLVERSION,(long)CURL_SSLVERSION_TLSv1_2);
    SET(CURLOPT_FRESH_CONNECT,1L);
    SET(CURLOPT_FORBID_REUSE,1L);
    SET(CURLOPT_SSL_SESSIONID_CACHE,0L);
    SET(CURLOPT_NOSIGNAL,1L);
    SET(CURLOPT_CONNECTTIMEOUT_MS,1500L);
    SET(CURLOPT_TIMEOUT_MS,4000L);
    SET(CURLOPT_WRITEFUNCTION,collect);
    SET(CURLOPT_WRITEDATA,&body);
#undef SET
    CURLcode performed=curl_easy_perform(easy);
    long status=-1;
    if (curl_easy_getinfo(easy,CURLINFO_RESPONSE_CODE,&status)!=CURLE_OK) goto done;
    if (reject) {
        if (performed==CURLE_PEER_FAILED_VERIFICATION && status==0 && body.used==0) result=0;
    } else {
        char *ip=NULL;
        long remote=0;
        if (curl_easy_getinfo(easy,CURLINFO_PRIMARY_IP,&ip)!=CURLE_OK ||
            curl_easy_getinfo(easy,CURLINFO_PRIMARY_PORT,&remote)!=CURLE_OK) goto done;
        if (performed==CURLE_OK && status==200 && ip && strcmp(ip,"127.0.0.1")==0 &&
            remote==port && body.used==sizeof(payload)-1 &&
            memcmp(body.bytes,payload,sizeof(payload)-1)==0) result=0;
    }
done:
    if (easy) curl_easy_cleanup(easy);
    curl_slist_free_all(resolve);
    curl_global_cleanup();
    return result;
}
