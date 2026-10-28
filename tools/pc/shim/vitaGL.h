/* PC test machine (tools/pc/nrpc.c): vitaGL's few calls of its own over Mesa's GLES 2 */
#ifndef NRPC_VITAGL_H
#define NRPC_VITAGL_H
#include "pc_gl.h"
#include "vitasdk.h"
enum { VGL_MEM_VRAM = 0, VGL_MEM_RAM, VGL_MEM_SLOW, VGL_MEM_BUDGET, VGL_MEM_EXTERNAL, VGL_MEM_ALL };
int vglInitExtended(int legacy_pool_size, int width, int height, int ram_threshold, SceGxmMultisampleMode msaa);
void vglSwapBuffers(GLboolean has_commondialog);
void vglWaitVblankStart(GLboolean enable);
size_t vglMemFree(int type);
#endif
