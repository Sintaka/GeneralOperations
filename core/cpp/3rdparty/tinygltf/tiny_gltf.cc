// MinGW GCC 8.1 advertises C++17 but its <filesystem> header is uncompilable;
// nlohmann/json otherwise includes it only for path conversion helpers tinygltf does not use.
#if defined(__GNUC__) && __GNUC__ == 8 && defined(_WIN32)
#define JSON_HAS_CPP_14
#endif
#define TINYGLTF_IMPLEMENTATION
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "tiny_gltf.h"
