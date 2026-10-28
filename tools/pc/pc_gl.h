/* PC (Mesa, headless GLES) stand-in for vitaGL, to look at the renderer's output */
#ifndef PC_GL_H
#define PC_GL_H
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#ifndef GL_DEPTH24_STENCIL8
#define GL_DEPTH24_STENCIL8 0x88F0
#endif
#ifndef GL_COMPRESSED_RGB_S3TC_DXT1_EXT
#define GL_COMPRESSED_RGB_S3TC_DXT1_EXT 0x83F0
#define GL_COMPRESSED_RGBA_S3TC_DXT5_EXT 0x83F3
#endif
/* vitaGL's GLSL front end turns these names into others with plain #defines (its
 * glsl_translator_hdr.h): "t" becomes "y", "s" becomes "x"... A shader with a variable t and a
 * variable y compiles here but fails on the Vita (v23 and v24's loading screen). The PC test
 * machine puts the same #defines in front of every shader, so such a clash fails here too. */
#define NRPC_VGL_MACROS "#define s x\n#define t y\n#define st xy\n#define ts yx\n#define ss xx\n#define tt yy\n" \
    "#define vgl varying\n#define matrix _matrix\n#define Matrix _Matrix\n#define sampler _sampler\n" \
    /* and Cg's own keywords, which GLSL lets a shader use as names: an error here too */ \
    "#define auto asm\n" "#define catch asm\n" "#define char asm\n" "#define column_major asm\n" "#define compile asm\n" "#define const_cast asm\n" "#define decl asm\n" "#define delete asm\n" "#define dword asm\n" "#define dynamic_cast asm\n" "#define emit asm\n" "#define explicit asm\n" "#define friend asm\n" "#define get asm\n" "#define mutable asm\n" "#define new asm\n" "#define operator asm\n" "#define pass asm\n" "#define pixelfragment asm\n" "#define pixelshader asm\n" "#define private asm\n" "#define protected asm\n" "#define register asm\n" "#define reinterpret_cast asm\n" "#define row_major asm\n" "#define sampler_state asm\n" "#define shared asm\n" "#define signed asm\n" "#define static_cast asm\n" "#define string asm\n" "#define technique asm\n" "#define texture asm\n" "#define texture1D asm\n" "#define texture3D asm\n" "#define textureCUBE asm\n" "#define textureRECT asm\n" "#define throw asm\n" "#define try asm\n" "#define typeid asm\n" "#define typename asm\n" "#define vector asm\n" "#define vertexfragment asm\n" "#define vertexshader asm\n" "#define virtual asm\n"
#endif
