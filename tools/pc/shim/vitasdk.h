/* PC test machine (tools/pc/nrpc.c): the small part of the VitaSDK that vita/main.c uses */
#ifndef NRPC_VITASDK_H
#define NRPC_VITASDK_H
#include <stdint.h>
#include <stddef.h>
typedef uint64_t SceUInt64;
typedef int SceUID;
typedef unsigned int SceSize;
typedef struct { uint64_t timeStamp; unsigned int buttons; unsigned char lx, ly, rx, ry; unsigned char up, right, down, left;
                 unsigned char lt, rt, l1, r1, triangle, circle, cross, square; unsigned char reserved[4]; } SceCtrlData;
enum { SCE_CTRL_SELECT = 0x1, SCE_CTRL_L3 = 0x2, SCE_CTRL_R3 = 0x4, SCE_CTRL_START = 0x8, SCE_CTRL_UP = 0x10, SCE_CTRL_RIGHT = 0x20,
       SCE_CTRL_DOWN = 0x40, SCE_CTRL_LEFT = 0x80, SCE_CTRL_LTRIGGER = 0x100, SCE_CTRL_RTRIGGER = 0x200, SCE_CTRL_L1 = 0x400,
       SCE_CTRL_R1 = 0x800, SCE_CTRL_TRIANGLE = 0x1000, SCE_CTRL_CIRCLE = 0x2000, SCE_CTRL_CROSS = 0x4000, SCE_CTRL_SQUARE = 0x8000 };
enum { SCE_CTRL_MODE_DIGITAL = 0, SCE_CTRL_MODE_ANALOG = 1, SCE_CTRL_MODE_ANALOG_WIDE = 2 };
typedef enum { SCE_GXM_MULTISAMPLE_NONE = 0, SCE_GXM_MULTISAMPLE_2X = 1, SCE_GXM_MULTISAMPLE_4X = 2 } SceGxmMultisampleMode;
typedef struct { int st_mode; unsigned int st_attr; int64_t st_size; } SceIoStat;
#define SCE_KERNEL_CPU_MASK_USER_0 0x10000
int sceCtrlPeekBufferPositive(int port, SceCtrlData *pad, int count);
typedef struct { unsigned char id, force; unsigned short x, y; unsigned char reserved[8]; unsigned short info; } SceTouchReport;
typedef struct { uint64_t timeStamp; unsigned int status; unsigned int reportNum; SceTouchReport report[8]; } SceTouchData;
enum { SCE_TOUCH_PORT_FRONT = 0, SCE_TOUCH_PORT_BACK = 1 };
enum { SCE_TOUCH_SAMPLING_STATE_STOP = 0, SCE_TOUCH_SAMPLING_STATE_START = 1 };
int sceTouchSetSamplingState(int port, int state);
int sceTouchPeek(int port, SceTouchData *data, int count);
int sceCtrlSetSamplingMode(int mode);
SceUInt64 sceKernelGetProcessTimeWide(void);
int sceKernelExitProcess(int res);
int sceKernelGetThreadId(void);
int sceKernelChangeThreadCpuAffinityMask(int thid, int mask);
int sceIoMkdir(const char *dir, int mode);
int sceIoGetstat(const char *file, SceIoStat *stat);
#endif
