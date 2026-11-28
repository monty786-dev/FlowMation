/* FlowBrowser - minimal PNG writer (portable). Used by the Wine / MSHTML host for screenshots: that engine has no
   DevTools protocol, so the host grabs the window's pixels itself. Output is valid but uncompressed (stored deflate). */
#ifndef FB_PNG_H
#define FB_PNG_H

#include "fb_util.h"

/* pixels: top-down rows of 4 bytes each in B,G,R,A order (a Windows 32-bit DIB). stride = bytes per row.
   Alpha is ignored (the PNG is opaque RGB). 0 = ok, appends the file to out. */
int fbpng_encode_bgra(const unsigned char *pixels, int width, int height, int stride, FbBuf *out);

#endif
