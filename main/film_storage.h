/*
 * film_storage：挂载板载 NAND（与 Vibe Mode 相同的 LittleFS，挂在 /nand）并准备照片目录。
 *
 * 只读写 /nand/DCIM/MOSAICO；绝不格式化，也不碰 /nand/system-update（系统更新包）。
 * 挂载失败时相机照常可用，只是不能存照片（界面提示"存储不可用"）。
 *
 * 单例：NAND 总线与挂载点是整机唯一资源，进程内只挂一次，之后一直保持挂载。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FILM_STORAGE_MOUNT "/nand"
#define FILM_STORAGE_PHOTO_DIR FILM_STORAGE_MOUNT "/DCIM/MOSAICO"

/** 挂载并创建照片目录（可重复调用，已挂载时直接返回） */
esp_err_t film_storage_mount(void);
/** 照片目录，未挂载时为 NULL */
const char *film_storage_photo_dir(void);
/**
 * 剩余空间（字节），未挂载时为 0。读的是缓存，立刻返回、任何任务（包括界面渲染）都可以调：
 * 现查要遍历文件系统并等它的锁，开机后台整理或写盘期间曾让一帧界面卡住几十秒。
 * 缓存由 film_storage_refresh_free 在后台任务里更新（挂载后、预热后、每张照片写完后）。
 */
uint64_t film_storage_free_bytes(void);
/** 现查剩余空间并更新缓存（会等文件系统，只在后台任务里调用） */
void film_storage_refresh_free(void);
/**
 * 预热空闲块表：LittleFS 每次挂载后第一次分配块时要遍历整个卷（数秒），
 * 在后台任务里写删一个小文件，让这次遍历不落在第一张照片上。未挂载时什么也不做。
 */
void film_storage_warm_up(void);

#ifdef __cplusplus
}
#endif
