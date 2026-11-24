/* A stand-in for vitaGL's source/shared.h on the PC: only what its GLSL translator (source/utils/glsl_utils.c and
 * the preprocessor beside it) uses. See build.sh. */
#ifndef SHARED_H
#define SHARED_H
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef unsigned char GLboolean; typedef unsigned int GLenum; typedef int GLsizei;
#define GL_TRUE 1
#define GL_FALSE 0
#define GL_VERTEX_SHADER 0x8B31
#define GL_FRAGMENT_SHADER 0x8B30
#define SCE_GXM_MAX_TEXTURE_UNITS 16
#define VERTEX_ATTRIBS_NUM 16
typedef enum { VGL_TYPE_NONE, VGL_TYPE_TEXCOORD, VGL_TYPE_TEXCOORD_CENTROID, VGL_TYPE_COLOR, VGL_TYPE_COLOR_CENTROID, VGL_TYPE_FOG, VGL_TYPE_FOG_CENTROID, VGL_TYPE_CLIP } vglSemanticType;
typedef enum { VGL_MODE_SHADER_PAIR, VGL_MODE_GLOBAL, VGL_MODE_POSTPONED } vglSemanticMode;
#define MAX_CG_TEXCOORD_ID 10
#define MAX_CG_COLOR_ID 2
typedef struct { char texcoord_names[MAX_CG_TEXCOORD_ID][64]; char color_names[MAX_CG_COLOR_ID][64]; GLboolean texcoord_used[MAX_CG_TEXCOORD_ID]; GLboolean color_used[MAX_CG_COLOR_ID]; } binds_map;
typedef struct { GLenum type; GLboolean is_glsl; binds_map semantics; const void *prog; uint32_t size; char *source; } shader;
#define vglMalloc malloc
#define vgl_free free
#define vgl_realloc realloc
#define vgl_fast_memcpy memcpy
#define vgl_memset memset
#define vgl_log printf
#define min(a, b) ((a) < (b) ? (a) : (b))
#endif
