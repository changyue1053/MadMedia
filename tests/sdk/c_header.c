#include <madmedia/madmedia_c.h>
int main(void) {
 madmedia_handle h=42;
 if(madmedia_abi_version()!=1)return 1;
 if(madmedia_create(NULL,&h)!=MADMEDIA_INVALID||h!=0)return 2;
 if(madmedia_destroy(0)!=MADMEDIA_CLOSED)return 3;
 madmedia_config_v2 v2={0};
 v2.struct_size=sizeof(v2);v2.abi_version=MADMEDIA_CONFIG_V2_VERSION;
 if(madmedia_create_v2(&v2,&h)!=MADMEDIA_INVALID||h!=0)return 4;
 return 0;
}
