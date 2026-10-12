# Image dimension guard: patch spec for `orbit/image.cpp`

SPDX-License-Identifier: GPL-3.0-or-later

Status: **specification only.** `wings/app` is not edited by the art module. Apply this when
integrating (README, "Exact app edits", item 6).

## Why
`orbit::image::decode` refuses images above `kMaxSourceSide = 8192` per side, but

* for PNG/JPEG it calls `stbi_load_from_memory` **first** and checks the size afterwards, so the
  full RGBA buffer (up to 8192 x 8192 x 4 = 268 MB) is already allocated against a 128 MiB heap;
* for WebP the check is correct (`WebPGetFeatures` first) but the limit is the same 8192.

The art module already refuses such files before they are cached (`check_image`, 4 Mpx / 4096 px,
`wings/art/include/wings/art/image_probe.hpp`), but `decode` is also fed by other sources (Pegasus,
installed-game icons, a disk cache written by an older version) so it must defend itself too.

## Patch (to `wings/app/src/orbit/image.cpp`)

```diff
@@ extern "C"
     unsigned char *stbi_load_from_memory(const unsigned char *buffer, int length, int *x, int *y,
                                          int *channels, int desired_channels);
+    int stbi_info_from_memory(const unsigned char *buffer, int len, int *x, int *y, int *comp);
     void stbi_image_free(void *pixels);
@@ namespace
-// Images larger than this in either direction are refused before decoding:
-// a hostile or broken file must not exhaust the fixed heap.
-constexpr int kMaxSourceSide = 8192;
+// Images larger than this are refused BEFORE any pixel buffer is allocated:
+// a hostile or broken file must not exhaust the fixed heap. 4 Mpx is 16 MB of
+// RGBA; PS CDN posters are 260 px, Pegasus covers are a few hundred.
+constexpr int kMaxSourceSide = 4096;
+constexpr long long kMaxSourcePixels = 4'000'000;
+
+bool size_allowed(int width, int height)
+{
+    return width > 0 && height > 0 && width <= kMaxSourceSide && height <= kMaxSourceSide &&
+           static_cast<long long>(width) * height <= kMaxSourcePixels;
+}
@@ decode_webp
-    if (width <= 0 || height <= 0 || width > kMaxSourceSide || height > kMaxSourceSide)
+    if (!size_allowed(width, height))
     {
         set_error(error, "WebP image size out of range");
@@ decode_stb
     if (bytes.size() > 0x7fffffff)
     { ... }
     int width = 0;
     int height = 0;
     int channels = 0;
+    // Header only: stb reads the PNG IHDR / JPEG SOF without allocating.
+    if (stbi_info_from_memory(reinterpret_cast<const unsigned char *>(bytes.data()),
+                              static_cast<int>(bytes.size()), &width, &height, &channels) == 0)
+    {
+        set_error(error, "not a readable image");
+        return false;
+    }
+    if (!size_allowed(width, height))
+    {
+        set_error(error, "image size out of range");
+        return false;
+    }
     unsigned char *pixels =
         stbi_load_from_memory(...);
@@
-    if (width > kMaxSourceSide || height > kMaxSourceSide)
-    {
-        stbi_image_free(pixels);
-        set_error(error, "image size out of range");
-        return false;
-    }
```

`stbi_info_from_memory` is part of the vendored `third_party/stb/image.c` when `STB_IMAGE_IMPLEMENTATION`
is compiled with PNG and JPEG enabled (it is a public stb_image function; confirm the app's build did
not strip it with `STBI_NO_*`; if it did, use `wings::art::probe_image` from this module instead).

## Tests to add (`tests/unit`)
* 5000 x 100 PNG header: `decode` returns false with "size out of range" and allocates nothing
  (check with a counting allocator or by running under the 128 MiB heap).
* 2500 x 2500 PNG header (6.25 Mpx): refused. 2000 x 2000 (4 Mpx exactly): accepted.
* 65535 x 65535 PNG IHDR in a 700-byte file: refused.
* Existing WebP/PNG/JPEG golden decodes unchanged.

## Related backpressure (ArtCache, same integration step)
`ArtCache::uploads_` is unbounded and `work_once()` keeps decoding while the frame thread uploads two
textures per frame. Bound it: keep an `std::atomic<int> pending_uploads_`; increment before
`inbox_.post(...)`, decrement when `tick()` pops; at the top of `work_once()` return false while
`pending_uploads_ >= 8`. (docs/tls-art.md, memory budget table.)
