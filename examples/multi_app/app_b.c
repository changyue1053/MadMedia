#define _DEFAULT_SOURCE
#include <madmedia/madmedia_c.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
int main(int argc,char** argv){
 if(argc!=3&&argc!=4)return 2;
 madmedia_config_v2 cfg={sizeof(cfg),2,"http://127.0.0.1:1",argv[2],2,0,MADMEDIA_JOURNAL_FULL,0};
 madmedia_handle h=0;int result=3;char id[MADMEDIA_TASK_ID_CAPACITY],buf[8192];size_t n=0;
 if(madmedia_create_v2(&cfg,&h)!=MADMEDIA_OK)return result;
 if(strcmp(argv[1],"foreign")==0&&argc==4){result=madmedia_query_persisted(h,argv[3],buf,sizeof(buf),&n)==MADMEDIA_NOT_FOUND?0:4;goto done;}
 if(strcmp(argv[1],"create")!=0||argc!=3){result=5;goto done;}
 size_t cap=strlen(argv[2])+16;char* output=malloc(cap);if(!output)goto done;
 snprintf(output,cap,"%s/output",argv[2]);madmedia_status submitted=madmedia_download(h,"fixture",output,id,sizeof(id));free(output);
 if(submitted!=MADMEDIA_OK){result=6;goto done;}
 if(madmedia_cancel(h,id)!=MADMEDIA_OK){result=7;goto done;}
 result=8;
 for(int i=0;i<20000;++i){
  madmedia_status s=madmedia_poll(h,buf,sizeof(buf),&n);
  if(s==MADMEDIA_OK&&strstr(buf,"\"terminal\":true")){
   if(!strstr(buf,"\"journal_receipt\":2")){result=9;break;}
   if(madmedia_query_persisted(h,id,buf,sizeof(buf),&n)!=MADMEDIA_OK){result=10;break;}
   result=0;break;
  }
  if(s!=MADMEDIA_OK&&s!=MADMEDIA_EMPTY){result=11;break;}
  usleep(1000);
 }
 if(result==0)puts(id);
done:
 if(madmedia_destroy(h)!=MADMEDIA_OK)return 12;
 return result;
}
