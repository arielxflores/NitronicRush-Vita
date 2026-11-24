#!/bin/bash
# build.sh VITAGL_DIR [OUT_DIR]: builds vglsl (see vglsl.c) from a vitaGL checkout (the commit the game is built
# with: docs/building.md). Only its GLSL translator and preprocessor are compiled, against shared.h here.
set -e
V=${1:?the vitaGL checkout}; O=${2:-build/vglsl}; H=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$O/w/utils" "$O/w/shaders" "$O/w/stub"
cp "$V/source/utils/glsl_utils.c" "$V/source/utils/glsl_utils.h" "$O/w/utils/"
cp -r "$V/source/utils/preprocessor" "$O/w/utils/"; rm -f "$O/w/utils/preprocessor/"*.o
cp "$V/source/shaders/glsl_translator_hdr.h" "$O/w/shaders/"
cp "$H/shared.h" "$H/vglsl.c" "$O/w/"
printf '#include <stdio.h>\n#define vgl_log printf\n' > "$O/w/stub/vitaGL.h"; echo '#include <stdio.h>' > "$O/w/stub/vitasdk.h"; : > "$O/w/utils/debug_utils.h"
cd "$O/w"
g++ -O1 -w -Istub -I. -c utils/preprocessor/preprocessor.cpp -o pp.o
g++ -O1 -w -Istub -I. -c utils/preprocessor/expression.cpp -o ex.o
gcc -O1 -w -std=gnu11 -I. -c utils/glsl_utils.c -o gu.o
gcc -O1 -std=gnu11 -I. -c vglsl.c -o vglsl.o
g++ pp.o ex.o gu.o vglsl.o -o ../vglsl
echo "built $O/vglsl"
