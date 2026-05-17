// Phase F.0.11.5 — tinyexr 实现文件 (C++ only).
//   tinyexr 内部用 std::vector / iostream, 必须 .cpp 编译.
//   STB_ZLIB 模式: 复用 stb_image.h 的 stbi_zlib_decode_buffer 与
//   stb_image_write.h 的 stbi_zlib_compress (已由 stb_impl.c 实现).
//
// 编译选项:
//   - TINYEXR_USE_MINIZ = 0  (避免再带一份 miniz, 减少符号重复)
//   - TINYEXR_USE_STB_ZLIB = 1  (复用现有 stb zlib 实现)
//   - TINYEXR_USE_THREAD = 0    (单线程, 截图频率低不必并行)
//   - TINYEXR_USE_OPENMP = 0    (避免引入 OpenMP 依赖)
//
// 参考: https://github.com/syoyo/tinyexr

#define TINYEXR_USE_MINIZ     0
#define TINYEXR_USE_STB_ZLIB  1
#define TINYEXR_USE_THREAD    0
#define TINYEXR_USE_OPENMP    0
#define TINYEXR_IMPLEMENTATION

#ifdef _MSC_VER
#  pragma warning(push)
#  pragma warning(disable: 4996)   // 'strcpy' deprecated (tinyexr 内大量使用)
#  pragma warning(disable: 4244)   // possible loss of data
#  pragma warning(disable: 4267)   // size_t to int 转换
#  pragma warning(disable: 4505)   // unreferenced static
#endif

#include "tinyexr.h"

#ifdef _MSC_VER
#  pragma warning(pop)
#endif
