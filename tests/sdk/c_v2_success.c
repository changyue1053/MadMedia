#define _DEFAULT_SOURCE
#include <madmedia/madmedia_c.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
/* Pure-C successful FULL path. Own only this fresh mkdtemp fixture; no /tmp scan. */
int main(void) {
 const char* base=getenv("TMPDIR");if(!base)base="/tmp";
 if(base[0]!='/')return 1;
 size_t cap=strlen(base)+128;char* dir=malloc(cap);char* path=malloc(cap);
 if(!dir||!path){free(dir);free(path);return 1;}
 snprintf(dir,cap,"%s/madmedia-v2-c-XXXXXX",base);
 if(!mkdtemp(dir)){free(dir);free(path);return 1;}
 madmedia_config_v2 c={sizeof(c),2,"http://127.0.0.1:1",dir,1,0,MADMEDIA_JOURNAL_FULL,0};
 madmedia_handle h=0;char id[MADMEDIA_TASK_ID_CAPACITY],buf[8192];int result=2;
 snprintf(path,cap,"%s/out",dir);
 if(madmedia_create_v2(&c,&h)!=MADMEDIA_OK)goto cleanup;
 if(madmedia_download(h,"media",path,id,sizeof(id))!=MADMEDIA_OK)goto cleanup;
 if(madmedia_cancel(h,id)!=MADMEDIA_OK)goto cleanup;
 for(int i=0;i<5000;++i){
  size_t n=0;madmedia_status status=madmedia_poll(h,buf,sizeof(buf),&n);
  if(status==MADMEDIA_OK && strstr(buf,"\"terminal\":true")){
   result=strstr(buf,"\"journal_receipt\":2")?0:3;break;
  }
  if(status!=MADMEDIA_OK&&status!=MADMEDIA_EMPTY)goto cleanup;
  usleep(1000);
 }
cleanup:
 if(h&&madmedia_destroy(h)!=MADMEDIA_OK)result=4;
 /* Only exact known fixture paths after native shutdown. Never recursive deletion. */
 const char* files[]={"journal.db","journal.db-wal","journal.db-shm",".madmedia.lock","out"};
 for(size_t i=0;i<sizeof(files)/sizeof(files[0]);++i){snprintf(path,cap,"%s/%s",dir,files[i]);unlink(path);}
 snprintf(path,cap,"%s/staging",dir);rmdir(path);rmdir(dir);free(path);free(dir);return result;
}
