#import <UIKit/UIKit.h>
#import <ImageIO/ImageIO.h>
#include <TargetConditionals.h>
#if !TARGET_OS_SIMULATOR || !defined(__arm64__)
#error Image preview qualification requires an ARM64 Simulator application
#endif
#include <madmedia/ios_image_preview.h>
#include <vector>
#include <cstring>
#include "fixture_artist_check.h"

@interface PreviewAppDelegate : UIResponder <UIApplicationDelegate>
@property(nonatomic,strong) UIWindow *window;
@end
@implementation PreviewAppDelegate
- (BOOL)application:(UIApplication *)app didFinishLaunchingWithOptions:(NSDictionary *)options {
    (void)app; (void)options;
    self.window = [[UIWindow alloc] initWithFrame:UIScreen.mainScreen.bounds];
    self.window.rootViewController = [UIViewController new];
    [self.window makeKeyAndVisible];
    return YES;
}
@end
int main(int argc, char* argv[]) {
    @autoreleasepool { return UIApplicationMain(argc, argv, nil, NSStringFromClass(PreviewAppDelegate.class)); }
}

// Test-only helper, called only on this freshly encoded 96x48 fixture.
// Exif APP1 payload: little-endian TIFF, first IFD at 8, one ASCII
// Artist (0x013B), its NUL-terminated value at offset 26, no next IFD.
static const uint8_t fixtureArtistApp1[] = {
    0xff, 0xe1, 0x00, 0x39, 0x45, 0x78, 0x69, 0x66, 0x00, 0x00, 0x49, 0x49,
    0x2a, 0x00, 0x08, 0x00, 0x00, 0x00, 0x01, 0x00, 0x3b, 0x01, 0x02, 0x00,
    0x17, 0x00, 0x00, 0x00, 0x1a, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x53, 0x79, 0x6e, 0x74, 0x68, 0x65, 0x74, 0x69, 0x63, 0x20, 0x66, 0x69,
    0x78, 0x74, 0x75, 0x72, 0x65, 0x20, 0x6f, 0x6e, 0x6c, 0x79, 0x00,
};

static NSData *seedFixtureArtist(NSData *fresh) {
    const auto *src=static_cast<const uint8_t *>(fresh.bytes);
    const size_t n=fresh.length;
    if (n<6 || n>128*1024 || src[0]!=0xff || src[1]!=0xd8 ||
        src[n-2]!=0xff || src[n-1]!=0xd9) return nil;
    NSMutableData *seeded=[NSMutableData dataWithCapacity:n+sizeof fixtureArtistApp1];
    [seeded appendBytes:src length:2];
    BOOL inserted=NO;
    for (size_t at=2; n-at>=4;) {
        if (src[at]!=0xff) return nil;
        const uint8_t kind=src[at+1];
        // Fresh ImageIO JPEG header only. No standalone/reserved marker
        // accepted here; entropy-coded data is never interpreted or modified.
        if (kind<0xc0 || kind==0xff || (kind>=0xd0 && kind<=0xd9)) return nil;
        const size_t length=static_cast<size_t>(src[at+2])*256+src[at+3];
        if (length<2 || length>n-at-2) return nil;
        if (!inserted && kind!=0xe0) {
            [seeded appendBytes:fixtureArtistApp1 length:sizeof fixtureArtistApp1];
            inserted=YES;
        }
        if (kind==0xda) {
            if (at+2+length>=n-2) return nil;
            [seeded appendBytes:src+at length:n-at];
            return seeded;
        }
        // Prevent duplicate Exif IFDs if the local encoder supplied its own.
        if (!(kind==0xe1 && length>=8 && std::memcmp(src+at+4,"Exif\0\0",6)==0))
            [seeded appendBytes:src+at length:2+length];
        at+=2+length;
    }
    return nil;
}

static NSData *fixture() {
    std::vector<uint8_t> rgba(96*48*4);
    for (unsigned y=0;y<48;++y) for (unsigned x=0;x<96;++x) {
        auto *p=&rgba[(y*96+x)*4]; p[0]=x<48?220:30;
        p[1]=(uint8_t)(60+((x*19+y*31)%100)); p[2]=x<48?30:220; p[3]=255;
    }
    CGColorSpaceRef color=CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    if (!color) return nil;
    CGContextRef ctx=CGBitmapContextCreate(rgba.data(),96,48,8,96*4,color,
                                          kCGImageAlphaNoneSkipLast|kCGBitmapByteOrder32Big);
    CGColorSpaceRelease(color);
    if (!ctx) return nil;
    CGImageRef image=CGBitmapContextCreateImage(ctx);
    CGContextRelease(ctx);
    if (!image) return nil;
    NSMutableData *data=[NSMutableData data];
    CGImageDestinationRef dst=CGImageDestinationCreateWithData((__bridge CFMutableDataRef)data,CFSTR("public.jpeg"),1,NULL);
    BOOL ok=NO;
    if (dst) {
        NSDictionary *options=@{(__bridge NSString *)kCGImageDestinationLossyCompressionQuality:@0.98};
        CGImageDestinationAddImage(dst,image,(__bridge CFDictionaryRef)options);
        ok=CGImageDestinationFinalize(dst);
        CFRelease(dst);
    }
    CGImageRelease(image);
    return ok?seedFixtureArtist(data):nil;
}

static NSDictionary *inspectJpeg(NSData *data, size_t w, size_t h, BOOL content, BOOL *shape = nullptr) {
    if (shape) *shape=NO;
    if (!data) return nil;
    CGImageSourceRef src=CGImageSourceCreateWithData((__bridge CFDataRef)data,NULL);
    if (!src) return nil;
    NSDictionary *info=CFBridgingRelease(CGImageSourceCopyPropertiesAtIndex(src,0,NULL));
    CGImageRef img=CGImageSourceCreateImageAtIndex(src,0,NULL);
    BOOL ok=CGImageSourceGetType(src) && CFEqual(CGImageSourceGetType(src),CFSTR("public.jpeg")) &&
        img && CGImageGetWidth(img)==w && CGImageGetHeight(img)==h;
    if (shape) *shape=ok && info!=nil;
    if (ok && content) {
        std::vector<uint8_t> pixels(w*h*4);
        CGColorSpaceRef color=CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
        CGContextRef ctx=color?CGBitmapContextCreate(pixels.data(),w,h,8,w*4,color,
                               kCGImageAlphaNoneSkipLast|kCGBitmapByteOrder32Big):NULL;
        if (color) CGColorSpaceRelease(color);
        if (!ctx) ok=NO;
        else {
            CGContextDrawImage(ctx,CGRectMake(0,0,w,h),img);
            const auto *l=&pixels[(h/2*w+w/4)*4], *r=&pixels[(h/2*w+3*w/4)*4];
            ok=l[0]>145 && l[2]<100 && r[0]<100 && r[2]>145;
            CGContextRelease(ctx);
        }
    }
    if (img) CGImageRelease(img);
    CFRelease(src);
    return ok?info:nil;
}

static NSData *resize(NSData *input, uint32_t edge, uint32_t quality) {
    if (!input) return nil;
    size_t n=0;
    auto bytes=static_cast<const uint8_t *>(input.bytes);
    if (madmedia_ios_jpeg_resize_v1(bytes,input.length,edge,quality,NULL,0,&n)!=MADMEDIA_IMAGE_V1_CAPACITY ||
        n==0 || n>8*1024*1024) return nil;
    NSMutableData *out=[NSMutableData dataWithLength:n];
    size_t written=0;
    if (madmedia_ios_jpeg_resize_v1(bytes,input.length,edge,quality,static_cast<uint8_t *>(out.mutableBytes),n,&written)!=MADMEDIA_IMAGE_V1_OK ||
        written!=n) return nil;
    return out;
}

static BOOL intendedRuntime() {
    NSOperatingSystemVersion version=NSProcessInfo.processInfo.operatingSystemVersion;
    return version.majorVersion==26 && version.minorVersion==2;
}

extern "C" __attribute__((used,visibility("default"))) const char *madmedia_preview_roundtrip() {
    if (!intendedRuntime()) return "runtime";
    NSData *input=fixture();
    if (!input) return "fixture-encode";
    NSDictionary *before=inspectJpeg(input,96,48,NO);
    if (!before) return "input-decode-or-shape";
    NSString *tiff=(__bridge NSString *)kCGImagePropertyTIFFDictionary;
    NSString *artist=(__bridge NSString *)kCGImagePropertyTIFFArtist;
    if (fixture_artist::jpeg(static_cast<const uint8_t *>(input.bytes),input.length)!=fixture_artist::Status::sentinel)
        return "input-artist-bytes";
    // ImageIO may omit TIFF Artist on iOS. An exposed value must agree with
    // the independently validated wire field; omission is not lack of seed.
    if (before[tiff][artist] && ![before[tiff][artist] isEqual:@"Synthetic fixture only"])
        return "input-artist-property-mismatch";

    NSData *high=resize(input,32,95);
    if (!high) return "high-codec";
    NSData *low=resize(input,32,20);
    if (!low) return "low-codec";
    BOOL shape=NO;
    NSDictionary *after=inspectJpeg(high,32,16,YES,&shape);
    if (!after) return shape?"high-pixels":"high-decode-or-shape";
    NSDictionary *afterLow=inspectJpeg(low,32,16,YES,&shape);
    if (!afterLow) return shape?"low-pixels":"low-decode-or-shape";
    if (high.length<=low.length) return "quality-response";

    NSData *same=resize(input,128,95);
    if (!same) return "no-upscale-codec";
    NSDictionary *afterSame=inspectJpeg(same,96,48,YES,&shape);
    if (!afterSame) return shape?"no-upscale-pixels":"no-upscale-decode-or-shape";
    for (NSData *output in @[high,low,same]) {
        const auto artistBytes=fixture_artist::jpeg(static_cast<const uint8_t *>(output.bytes),output.length);
        if (artistBytes==fixture_artist::Status::invalid) return "output-artist-invalid";
        if (artistBytes!=fixture_artist::Status::absent) return "output-artist-bytes";
    }
    // Also reject any Artist the decoder independently exposes.
    if (after[tiff][artist] || afterLow[tiff][artist] || afterSame[tiff][artist])
        return "output-artist";
    return nullptr;
}

extern "C" __attribute__((used,visibility("default"))) BOOL madmedia_preview_contract() {
    if (!intendedRuntime()) return NO;
    NSData *input=fixture();
    if (!input) return NO;
    const uint8_t invalid[]={1,2,3,4};
    uint8_t sentinel=0x55;
    size_t n=1;
    if (madmedia_ios_jpeg_resize_v1(invalid,sizeof invalid,32,80,&sentinel,1,&n)!=MADMEDIA_IMAGE_V1_DECODE ||
        n || sentinel!=0x55) return NO;
    auto bytes=static_cast<const uint8_t *>(input.bytes);
    // Use real tiny buffers; logical limit checks must precede byte access.
    return madmedia_ios_jpeg_resize_v1(bytes,8*1024*1024+1,32,80,&sentinel,1,&n)==MADMEDIA_IMAGE_V1_LIMIT && !n &&
        madmedia_ios_jpeg_resize_v1(bytes,input.length,2049,80,&sentinel,1,&n)==MADMEDIA_IMAGE_V1_LIMIT && !n &&
        madmedia_ios_jpeg_resize_v1(bytes,input.length,32,101,&sentinel,1,&n)==MADMEDIA_IMAGE_V1_ARGUMENT && !n &&
        madmedia_ios_jpeg_resize_v1(bytes,input.length,32,80,&sentinel,1,&n)==MADMEDIA_IMAGE_V1_CAPACITY &&
        n>1 && sentinel==0x55;
}
