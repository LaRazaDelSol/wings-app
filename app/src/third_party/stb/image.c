// Wings TV app - The single compilation unit for the vendored stb_image.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// PNG and JPEG only: the catalogue's fallback artwork. WebP, the primary
// artwork format, is decoded by libwebp (src/orbit/image.cpp). Images arrive
// in memory from the Orbit backend, so stdio is left out.

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#include "stb_image.inc"
