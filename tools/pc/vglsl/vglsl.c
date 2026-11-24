/* vglsl VS.glsl FS.glsl|- : what vitaGL's GLSL translator makes of a pair of the game's shaders - the Cg the Vita's
 * shader compiler is handed (vitaGL translates when a program is linked: the vertex shader first, its varyings
 * numbered TEXCOORD0 on in the order they are declared; the fragment shader's found by name).
 *   FS "-": the fragment shader is Cg already (GL_CG_FRAGMENT_SHADER_EXT), as the half-precision trial's are - shows
 *           the semantics the vertex shader's varyings get then.
 *   VGLSL_FULL=1: the whole text with vitaGL's header, not only the shaders' own part.
 * The game's sources as the Vita gets them: SHDUMP=dir with the Linux build (docs/testing.md). Built by build.sh from
 * a vitaGL checkout; nothing of vitaGL is kept in this repository. */
#include "shared.h"
#include "utils/glsl_utils.h"
static char *slurp(const char *p)
{
    FILE *f = fopen(p, "rb"); if (!f) { perror(p); exit(1); }
    fseek(f, 0, SEEK_END); long n = ftell(f); rewind(f);
    char *s = malloc(n + 1); if (fread(s, 1, n, f) != (size_t)n) { perror(p); exit(1); } s[n] = 0; fclose(f);
    return s;
}
static const char *own(const char *src)            /* past vitaGL's header */
{
    static const char *end = "varying out float4 gl_FragColor : COLOR;\n#endif\n";
    const char *q = getenv("VGLSL_FULL") ? NULL : strstr(src, end);
    return q ? q + strlen(end) : src;
}
int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "vglsl VS.glsl FS.glsl|-\n"); return 1; }
    shader vs = { 0 }, fs = { 0 };
    vs.type = GL_VERTEX_SHADER; fs.type = GL_FRAGMENT_SHADER; vs.is_glsl = fs.is_glsl = 1;
    vs.source = slurp(argv[1]);
    if (strcmp(argv[2], "-")) fs.source = slurp(argv[2]); else { fs.prog = (void *)1; fs.is_glsl = 0; }
    glsl_sema_mode = VGL_MODE_SHADER_PAIR;         /* (as glLinkProgram sets it for the default, VGL_MODE_POSTPONED) */
    glsl_translator_set_process(&vs, &fs);
    printf("// ---- vertex shader\n%s\n", own(vs.source));
    if (fs.is_glsl || fs.source) printf("// ---- fragment shader\n%s\n", own(fs.source));
    return 0;
}
/* what the translator calls in the rest of vitaGL */
void *sceClibMemmove(void *d, const void *s, size_t n) { return memmove(d, s, n); }
void vglAddSemanticBinding(const char *varying, int index, GLenum type)
{
    strcpy(glsl_custom_bindings[glsl_custom_bindings_num].name, varying);
    glsl_custom_bindings[glsl_custom_bindings_num].idx = index;
    glsl_custom_bindings[glsl_custom_bindings_num].type = type;
    glsl_custom_bindings[glsl_custom_bindings_num++].ref_idx = glsl_current_ref_idx;
}
