#ifndef MADMEDIA_FIXTURE_ARTIST_CHECK_H
#define MADMEDIA_FIXTURE_ARTIST_CHECK_H
// Test-only first-IFD Artist inspection of the synthetic JPEG inputs/outputs.
// Not a generic metadata extractor, untrusted media API, or JPEG decoder.
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace fixture_artist {
enum class Status { invalid, absent, sentinel, other };
inline constexpr char value[] = "Synthetic fixture only";

inline Status tiff(const uint8_t *p, size_t n) {
    if (n<14) return Status::invalid;
    const bool little=p[0]=='I' && p[1]=='I';
    if (!little && !(p[0]=='M' && p[1]=='M')) return Status::invalid;
    auto get=[&](size_t at, size_t count) {
        uint32_t v=0;
        for (size_t i=0;i<count;++i) v=(v<<8)|p[at+(little?count-1-i:i)];
        return v;
    };
    if (get(2,2)!=42) return Status::invalid;
    const size_t ifd=get(4,4);
    if (ifd<8 || ifd>n-6) return Status::invalid;
    const size_t entries=get(ifd,2);
    if (entries>(n-ifd-6)/12) return Status::invalid;
    const size_t end=ifd+2+entries*12;
    // This fixture contract only accepts a single IFD0 image directory.
    if (get(end,4)!=0) return Status::invalid;
    Status found=Status::absent;
    for (size_t at=ifd+2;at<end;at+=12) {
        if (get(at,2)!=0x013b) continue;
        if (found!=Status::absent || get(at+2,2)!=2) return Status::invalid;
        const size_t count=get(at+4,4);
        const size_t at_value=count<=4?at+8:get(at+8,4);
        if (!count || (count>4 && at_value<end+4) ||
            at_value>n || count>n-at_value || p[at_value+count-1]!=0)
            return Status::invalid;
        found=count==sizeof value && std::memcmp(p+at_value,value,sizeof value)==0
            ? Status::sentinel : Status::other;
    }
    return found;
}

inline Status jpeg(const uint8_t *p,size_t n) {
    if (!p || n<6 || n>8*1024*1024 || p[0]!=0xff || p[1]!=0xd8 ||
        p[n-2]!=0xff || p[n-1]!=0xd9) return Status::invalid;
    bool exif=false, entropy=false, saw_scan=false, scan_data=false;
    Status found=Status::absent;
    for (size_t at=2;at<n;) {
        if (p[at]!=0xff) {
            if (!entropy) return Status::invalid;
            scan_data=true;
            ++at;
            continue;
        }
        const size_t marker=at++;
        while (at<n && p[at]==0xff) ++at; // marker fill
        if (at==n) return Status::invalid;
        const uint8_t kind=p[at++];
        if (kind==0x00) { // entropy-coded literal 0xff
            if (!entropy || at-marker!=2) return Status::invalid;
            scan_data=true;
            continue;
        }
        if (kind>=0xd0 && kind<=0xd7) { // RST0..RST7
            if (!entropy) return Status::invalid;
            continue;
        }
        if (entropy && !scan_data) return Status::invalid;
        entropy=false;
        if (kind==0xd9) return saw_scan && at==n?found:Status::invalid;
        if (kind<0xc0 || kind==0xd8 || n-at<2) return Status::invalid;
        const size_t len=static_cast<size_t>(p[at])*256+p[at+1];
        if (len<2 || len>n-at) return Status::invalid;
        if (kind==0xda) {
            if (len<6) return Status::invalid;
            saw_scan=true;
            entropy=true;
            scan_data=false;
        } else if (kind==0xe1 && len>=8 && std::memcmp(p+at+2,"Exif\0\0",6)==0) {
            if (exif) return Status::invalid;
            exif=true;
            found=tiff(p+at+8,len-8);
            if (found==Status::invalid) return found;
        }
        at+=len;
    }
    return Status::invalid;
}
}
#endif
