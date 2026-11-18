// stb 单头库的唯一实现单元（vendored 第三方，public domain）。
// 隔离在独立 TU 并局部关闭 -Werror：第三方代码不纳入本项目的告警纪律，
// 但只在此文件放开——core 其余部分维持 -Wall -Wextra -Wpedantic -Werror。
#if defined(__clang__)
#pragma clang diagnostic ignored "-Weverything"
#endif

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO          // 不用文件 IO：只从内存缓冲解码（沙箱纪律）
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#include "third_party/stb/stb_image.h"

#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include "third_party/stb/stb_image_resize2.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#include "third_party/stb/stb_image_write.h"

