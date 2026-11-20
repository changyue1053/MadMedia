#import <UIKit/UIKit.h>
#import <ImageIO/ImageIO.h>
#include <TargetConditionals.h>
#if !TARGET_OS_SIMULATOR || !defined(__arm64__)
#error Image preview qualification requires an ARM64 Simulator application
#endif
#include <madmedia/ios_image_preview.h>
#include <vector>

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
        NSDictionary *metadata=@{(__bridge NSString *)kCGImagePropertyTIFFDictionary:
             @{(__bridge NSString *)kCGImagePropertyTIFFArtist:@"Synthetic fixture only"},
             (__bridge NSString *)kCGImageDestinationLossyCompressionQuality:@0.98};
        CGImageDestinationAddImage(dst,image,(__bridge CFDictionaryRef)metadata);
        ok=CGImageDestinationFinalize(dst);
        CFRelease(dst);
    }
    CGImageRelease(image);
    return ok?data:nil;
}

static NSDictionary *inspectJpeg(NSData *data, size_t w, size_t h, BOOL content) {
    if (!data) return nil;
    CGImageSourceRef src=CGImageSourceCreateWithData((__bridge CFDataRef)data,NULL);
    if (!src) return nil;
    NSDictionary *info=CFBridgingRelease(CGImageSourceCopyPropertiesAtIndex(src,0,NULL));
    CGImageRef img=CGImageSourceCreateImageAtIndex(src,0,NULL);
    BOOL ok=CGImageSourceGetType(src) && CFEqual(CGImageSourceGetType(src),CFSTR("public.jpeg")) &&
        img && CGImageGetWidth(img)==w && CGImageGetHeight(img)==h;
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

extern "C" __attribute__((used,visibility("default"))) BOOL madmedia_preview_roundtrip() {
    if (!intendedRuntime()) return NO;
    NSData *input=fixture();
    NSDictionary *before=inspectJpeg(input,96,48,NO);
    NSString *tiff=(__bridge NSString *)kCGImagePropertyTIFFDictionary;
    NSString *artist=(__bridge NSString *)kCGImagePropertyTIFFArtist;
    if (![before[tiff][artist] isEqual:@"Synthetic fixture only"]) return NO;
    NSData *high=resize(input,32,95), *low=resize(input,32,20);
    NSDictionary *after=inspectJpeg(high,32,16,YES);
    if (!after || !inspectJpeg(low,32,16,YES) || high.length<=low.length ||
        !inspectJpeg(resize(input,128,95),96,48,YES)) return NO;
    return !after[tiff][artist] && !after[(__bridge NSString *)kCGImagePropertyGPSDictionary] &&
        !after[(__bridge NSString *)kCGImagePropertyExifDictionary] &&
        !after[(__bridge NSString *)kCGImagePropertyIPTCDictionary];
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
