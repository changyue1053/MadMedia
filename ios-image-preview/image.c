#include <madmedia/ios_image_preview.h>
#include <CoreGraphics/CoreGraphics.h>
#include <ImageIO/ImageIO.h>
#include <string.h>

#define LIMIT_BYTES (8u * 1024u * 1024u)
static int dimension(CFDictionaryRef props, CFStringRef key, int64_t *result) {
  CFTypeRef v = CFDictionaryGetValue(props, key);
  return v && CFGetTypeID(v) == CFNumberGetTypeID() &&
      CFNumberGetValue((CFNumberRef)v, kCFNumberSInt64Type, result);
}
int32_t madmedia_ios_jpeg_resize_v1(const uint8_t *input, size_t size,
                                   uint32_t edge, uint32_t quality,
                                   uint8_t *output, size_t capacity, size_t *out_size) {
  if (!out_size) return MADMEDIA_IMAGE_V1_ARGUMENT;
  *out_size = 0;
  if (!input || !size || (!output && capacity) || !edge || !quality || quality > 100)
    return MADMEDIA_IMAGE_V1_ARGUMENT;
  if (size > LIMIT_BYTES || edge > 2048) return MADMEDIA_IMAGE_V1_LIMIT;
  if (size < 3 || input[0]!=0xff || input[1]!=0xd8 || input[2]!=0xff)
    return MADMEDIA_IMAGE_V1_DECODE;

  CFDataRef data = NULL;
  CGImageSourceRef src = NULL;
  CFDictionaryRef props = NULL, options = NULL, encode_options = NULL;
  CFNumberRef edge_num = NULL, quality_num = NULL;
  CGImageRef thumbnail = NULL, pixels = NULL;
  CGColorSpaceRef color = NULL;
  CGContextRef canvas = NULL;
  CFMutableDataRef encoded = NULL;
  CGImageDestinationRef dst = NULL;
  int32_t result = MADMEDIA_IMAGE_V1_DECODE;
  int64_t w=0, h=0;
  data=CFDataCreate(kCFAllocatorDefault,input,(CFIndex)size);
  if (!data) goto done;
  src=CGImageSourceCreateWithData(data,NULL);
  if (!src || !CGImageSourceGetType(src) || !CFEqual(CGImageSourceGetType(src),CFSTR("public.jpeg")) ||
      CGImageSourceGetCount(src)!=1 || CGImageSourceGetStatus(src)!=kCGImageStatusComplete) goto done;
  props=CGImageSourceCopyPropertiesAtIndex(src,0,NULL);
  if (!props || !dimension(props,kCGImagePropertyPixelWidth,&w) ||
      !dimension(props,kCGImagePropertyPixelHeight,&h) || w<=0 || h<=0) goto done;
  if (w>8192 || h>8192 || w*h>4000000) { result=MADMEDIA_IMAGE_V1_LIMIT; goto done; }
  int wanted=(int)edge;
  if (w<wanted && h<wanted) wanted=(int)(w>h?w:h);
  edge_num=CFNumberCreate(NULL,kCFNumberIntType,&wanted);
  if (!edge_num) goto done;
  const void *keys[]={kCGImageSourceCreateThumbnailFromImageAlways,kCGImageSourceCreateThumbnailWithTransform,
                      kCGImageSourceThumbnailMaxPixelSize,kCGImageSourceShouldCacheImmediately};
  const void *values[]={kCFBooleanTrue,kCFBooleanTrue,edge_num,kCFBooleanTrue};
  options=CFDictionaryCreate(NULL,keys,values,4,&kCFTypeDictionaryKeyCallBacks,&kCFTypeDictionaryValueCallBacks);
  if (!options) goto done;
  thumbnail=CGImageSourceCreateThumbnailAtIndex(src,0,options);
  if (!thumbnail) goto done;
  size_t tw=CGImageGetWidth(thumbnail), th=CGImageGetHeight(thumbnail);
  if (!tw || !th || tw>(size_t)wanted || th>(size_t)wanted ||
      tw*th>(size_t)(w*h)) goto done;

  /* Draw into a fresh sRGB raster; do not hand source, thumbnails' property
   * dictionaries, ICC input profile, EXIF/IPTC/XMP/GPS to the destination. */
  result=MADMEDIA_IMAGE_V1_ENCODE;
  color=CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
  if (!color) goto done;
  canvas=CGBitmapContextCreate(NULL,tw,th,8,tw*4,color,
                               kCGImageAlphaNoneSkipLast | kCGBitmapByteOrder32Big);
  if (!canvas) goto done;
  CGContextDrawImage(canvas,CGRectMake(0,0,tw,th),thumbnail);
  pixels=CGBitmapContextCreateImage(canvas);
  encoded=CFDataCreateMutable(NULL,0);
  if (!pixels || !encoded) goto done;
  dst=CGImageDestinationCreateWithData(encoded,CFSTR("public.jpeg"),1,NULL);
  if (!dst) goto done;
  double q=(double)quality/100.0;
  quality_num=CFNumberCreate(NULL,kCFNumberDoubleType,&q);
  if (!quality_num) goto done;
  const void *qkey[]={kCGImageDestinationLossyCompressionQuality}, *qvalue[]={quality_num};
  encode_options=CFDictionaryCreate(NULL,qkey,qvalue,1,&kCFTypeDictionaryKeyCallBacks,&kCFTypeDictionaryValueCallBacks);
  if (!encode_options) goto done;
  CGImageDestinationAddImage(dst,pixels,encode_options);
  if (!CGImageDestinationFinalize(dst)) goto done;
  CFIndex count=CFDataGetLength(encoded);
  if (count<=0 || count>(CFIndex)LIMIT_BYTES) goto done;
  *out_size=(size_t)count;
  if (capacity<(size_t)count) { result=MADMEDIA_IMAGE_V1_CAPACITY; goto done; }
  memcpy(output,CFDataGetBytePtr(encoded),(size_t)count);
  result=MADMEDIA_IMAGE_V1_OK;
done:
  if (dst) CFRelease(dst);
  if (encode_options) CFRelease(encode_options);
  if (quality_num) CFRelease(quality_num);
  if (encoded) CFRelease(encoded);
  if (pixels) CGImageRelease(pixels);
  if (canvas) CGContextRelease(canvas);
  if (color) CGColorSpaceRelease(color);
  if (thumbnail) CGImageRelease(thumbnail);
  if (options) CFRelease(options);
  if (edge_num) CFRelease(edge_num);
  if (props) CFRelease(props);
  if (src) CFRelease(src);
  if (data) CFRelease(data);
  return result;
}
