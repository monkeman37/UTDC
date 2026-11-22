/* Single translation unit for the header-only third-party libraries. */
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#include "../third_party/stb/stb_image.h"

#undef STB_VORBIS_HEADER_ONLY
#define STB_VORBIS_NO_STDIO
#define STB_VORBIS_NO_PUSHDATA_API
#include "../third_party/stb/stb_vorbis.c"
