#include <madmedia/madmedia_c.h>
int main(void) {
 madmedia_handle h=42;
 if(madmedia_abi_version()!=1)return 1;
 if(madmedia_create(NULL,&h)!=MADMEDIA_INVALID||h!=0)return 2;
 if(madmedia_destroy(0)!=MADMEDIA_CLOSED)return 3;
 return 0;
}
