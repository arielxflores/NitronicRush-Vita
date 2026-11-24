cmake_minimum_required(VERSION 3.10)
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(DEFINED ENV{VITASDK})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
  else()
    message(FATAL_ERROR "Please define VITASDK to point to your SDK path!")
  endif()
endif()

project(nitronic_vita C CXX)
include("$ENV{VITASDK}/share/vita.cmake" REQUIRED)

set(VITA_APP_NAME "Nitronic Rush")
set(VITA_TITLEID  "NRSH00001")
set(VITA_VERSION  "01.10")
if(NOT DEFINED NR_BUILD_LABEL)
  set(NR_BUILD_LABEL "1.1")
endif()

# (meshoptimizer, the objects' simpler shapes at load: C++ without exceptions)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O2 -fno-exceptions -fno-rtti -mtune=cortex-a9 -mfpu=neon")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O2 -std=gnu99 -Wall -Wno-misleading-indentation -ffast-math -mtune=cortex-a9 -mfpu=neon -DNR_BUILD_LABEL=\\\"${NR_BUILD_LABEL}\\\"")

add_executable(nitronic vita/third_party/meshoptimizer/simplifier.cpp vita/third_party/meshoptimizer/allocator.cpp vita/main.c vita/hud.c vita/gfx.c vita/front.c vita/save.c vita/snd.c vita/particles.c vita/camera.c vita/ghost.c vita/scores.c vita/chase.c vita/results.c vita/cinema.c vita/io.c vita/gputime.c src/nr_pak.c src/nr_data.c src/nr_coll.c src/nr_car.c src/nr_tricks.c src/nr_objs.c src/tinf/tinflate.c src/libdeflate/lib/deflate_decompress.c src/libdeflate/lib/utils.c)
# (the sound mixer and its Vorbis decoder: -O3, which unrolls and vectorises their inner loops with NEON)
set_source_files_properties(vita/snd.c PROPERTIES COMPILE_FLAGS "-O3")
target_link_libraries(nitronic
  vitaGL vitashark SceShaccCgExt mathneon taihen_stub SceShaccCg_stub
  SceKernelDmacMgr_stub SceGxm_stub SceDisplay_stub SceCtrl_stub SceTouch_stub SceMotion_stub
  SceAppMgr_stub SceAppUtil_stub SceCommonDialog_stub SceSysmodule_stub SceIme_stub
  SceIofilemgr_stub SceLibKernel_stub SceKernelThreadMgr_stub SceAudio_stub ScePower_stub
  stdc++ m c)
# vitaGL contains C++ (its shader preprocessor), so link with the C++ driver and runtime
set_target_properties(nitronic PROPERTIES LINKER_LANGUAGE CXX)
# (room for vita-elf-create's module data at the code segment's end: vita/textpad.ld)
set_property(TARGET nitronic APPEND_STRING PROPERTY LINK_FLAGS " -Wl,-T,${CMAKE_CURRENT_SOURCE_DIR}/vita/textpad.ld")

vita_create_self(eboot.bin nitronic UNSAFE)
# (the built-in shader programs, when made: tools/shbank.py)
set(NR_VPK_EXTRA "")
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/vita/shbank.bin)
  set(NR_VPK_EXTRA FILE vita/shbank.bin shbank.bin)
endif()
vita_create_vpk(nitronic.vpk ${VITA_TITLEID} eboot.bin
  VERSION ${VITA_VERSION}
  NAME ${VITA_APP_NAME}
  FILE vita/sce_sys/icon0.png sce_sys/icon0.png
  FILE vita/sce_sys/livearea/contents/bg.png sce_sys/livearea/contents/bg.png
  FILE vita/sce_sys/livearea/contents/bg0.png sce_sys/livearea/contents/bg0.png
  FILE vita/sce_sys/livearea/contents/frame1.png sce_sys/livearea/contents/frame1.png
  FILE vita/sce_sys/livearea/contents/startup.png sce_sys/livearea/contents/startup.png
  FILE vita/sce_sys/livearea/contents/template.xml sce_sys/livearea/contents/template.xml
  ${NR_VPK_EXTRA}
)
