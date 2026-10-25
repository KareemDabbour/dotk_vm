// Single translation unit for the vendored stb_image.h (JPEG, PNG, BMP, GIF and TGA decoding from memory).
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_NO_PSD
#define STBI_NO_PIC
#define STBI_NO_PNM
#define STBI_MAX_DIMENSIONS (1 << 15)
#include "stb_image.h"
