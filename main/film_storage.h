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
 * 剩余空间（字节）：恒为 0，表示"未知"。
 *
 * LittleFS 算已用空间（lfs_fs_size）要把整个卷的每一个数据块都读一遍（每块一次 NAND 读），
 * 耗时随照片数线性增长（一百多张照片就是几十秒），而且全程占着文件系统的锁：
 * 放在挂载里开机多等几十秒，放在界面里整个界面卡死，放在写盘任务里拍照后看门狗复位。
 * 所以任何地方都不再查它；容量写满由写文件时的 ENOSPC 报 FILM_ERR_STORAGE_FULL。
 */
uint64_t film_storage_free_bytes(void);
/** 存储总容量（字节），读块设备几何信息，不碰文件系统；未挂载时为 0。任何任务都可以调 */
uint64_t film_storage_total_bytes(void);
/**
 * 预热空闲块表：LittleFS 每次挂载后第一次分配块时要遍历整个卷（数秒），
 * 在后台任务里写删一个小文件，让这次遍历不落在第一张照片上。未挂载时什么也不做。
 */
void film_storage_warm_up(void);

#ifdef __cplusplus
}
#endif
