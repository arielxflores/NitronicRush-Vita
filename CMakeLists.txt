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
set(VITA_VERSION  "01.00")
if(NOT DEFINED NR_BUILD_LABEL)
  set(NR_BUILD_LABEL "1.0")
endif()

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O2 -std=gnu99 -Wall -Wno-misleading-indentation -ffast-math -mtune=cortex-a9 -mfpu=neon -DNR_BUILD_LABEL=\\\"${NR_BUILD_LABEL}\\\"")

add_executable(nitronic vita/main.c vita/hud.c vita/gfx.c vita/front.c vita/save.c vita/snd.c vita/particles.c vita/camera.c vita/ghost.c vita/scores.c vita/chase.c vita/results.c vita/cinema.c vita/io.c src/nr_pak.c src/nr_data.c src/nr_coll.c src/nr_car.c src/nr_tricks.c src/nr_objs.c src/tinf/tinflate.c)
target_link_libraries(nitronic
  vitaGL vitashark SceShaccCgExt mathneon taihen_stub SceShaccCg_stub
  SceKernelDmacMgr_stub SceGxm_stub SceDisplay_stub SceCtrl_stub SceTouch_stub SceMotion_stub
  SceAppMgr_stub SceAppUtil_stub SceCommonDialog_stub SceSysmodule_stub SceIme_stub
  SceIofilemgr_stub SceLibKernel_stub SceKernelThreadMgr_stub SceAudio_stub ScePower_stub
  stdc++ m c)
# vitaGL contains C++ (its shader preprocessor), so link with the C++ driver and runtime
set_target_properties(nitronic PROPERTIES LINKER_LANGUAGE CXX)

vita_create_self(eboot.bin nitronic UNSAFE)
vita_create_vpk(nitronic.vpk ${VITA_TITLEID} eboot.bin
  VERSION ${VITA_VERSION}
  NAME ${VITA_APP_NAME}
  FILE vita/sce_sys/icon0.png sce_sys/icon0.png
  FILE vita/sce_sys/livearea/contents/bg.png sce_sys/livearea/contents/bg.png
  FILE vita/sce_sys/livearea/contents/bg0.png sce_sys/livearea/contents/bg0.png
  FILE vita/sce_sys/livearea/contents/frame1.png sce_sys/livearea/contents/frame1.png
  FILE vita/sce_sys/livearea/contents/startup.png sce_sys/livearea/contents/startup.png
  FILE vita/sce_sys/livearea/contents/template.xml sce_sys/livearea/contents/template.xml
)
