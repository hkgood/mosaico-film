#include "film_perf.h"

#ifdef CONFIG_FILM_PERF_TRACE

#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define CPU_MAX_TASKS   40      /*!< 快照最多记录的任务数 */
#define CPU_TOP_TASKS   8       /*!< 每次只打印占用最高的几个 */
#define CPU_MIN_PERCENT 0.5f    /*!< 低于此占用的任务不打印（空闲任务除外） */

/* ---------------------------------------------------------------- 快门时间线 */

static int64_t s_event_us[FILM_PERF_COUNT];
static bool s_shot_active;

/** 两个事件之间的毫秒数，任一端缺失时为 -1 */
static int span_ms(film_perf_event_t from, film_perf_event_t to)
{
    return s_event_us[from] && s_event_us[to] ? (int)((s_event_us[to] - s_event_us[from]) / 1000) : -1;
}

void film_perf_mark(film_perf_event_t event)
{
    if (event >= FILM_PERF_COUNT) {
        return;
    }
    if (event == FILM_PERF_SHUTTER) {
        memset(s_event_us, 0, sizeof(s_event_us));
        s_shot_active = true;
    }
    if (s_shot_active) {
        s_event_us[event] = esp_timer_get_time();
    }
}

void film_perf_preview_frame(void)
{
    /* 只有原片已到手之后的第一帧才算"取景恢复"（拍照前后停流期间不会有帧） */
    if (!s_shot_active || !s_event_us[FILM_PERF_FRAME_READY]) {
        return;
    }
    s_event_us[FILM_PERF_PREVIEW_RESUMED] = esp_timer_get_time();
    s_shot_active = false;
    /*
     * 三段（停取景 / 切格式 / 曝光帧到达）合起来是"快门 → 原片 JPEG"；
     * interval 是"快门 → 取景恢复"，即用户能再拍下一张的间隔。
     */
    FILM_PERF_LOG("shot",
                  "grab_proof %d | stop_stream %d | switch_format %d | stream_on %d | frames %d "
                  "=> raw_ready %d | to_lab %d | proof %d | full %d | resume %d => interval %d ms",
                  span_ms(FILM_PERF_SHUTTER, FILM_PERF_PROOF_GRABBED),
                  span_ms(FILM_PERF_PROOF_GRABBED, FILM_PERF_STREAM_STOPPED),
                  span_ms(FILM_PERF_STREAM_STOPPED, FILM_PERF_FORMAT_SWITCHED),
                  span_ms(FILM_PERF_FORMAT_SWITCHED, FILM_PERF_STILL_STREAMING),
                  span_ms(FILM_PERF_STILL_STREAMING, FILM_PERF_FRAME_READY),
                  span_ms(FILM_PERF_SHUTTER, FILM_PERF_FRAME_READY),
                  span_ms(FILM_PERF_FRAME_READY, FILM_PERF_LAB_START),
                  span_ms(FILM_PERF_LAB_START, FILM_PERF_PROOF_DONE),
                  span_ms(FILM_PERF_PROOF_DONE, FILM_PERF_FULL_DONE),
                  span_ms(FILM_PERF_FULL_DONE, FILM_PERF_PREVIEW_RESUMED),
                  span_ms(FILM_PERF_SHUTTER, FILM_PERF_PREVIEW_RESUMED));
}

/* ---------------------------------------------------------------- 内存 */

static unsigned kb(size_t bytes)
{
    return (unsigned)(bytes / 1024);
}

void film_perf_mem(const char *tag)
{
    multi_heap_info_t psram, internal;
    heap_caps_get_info(&psram, MALLOC_CAP_SPIRAM);
    heap_caps_get_info(&internal, MALLOC_CAP_INTERNAL);
    FILM_PERF_LOG("mem",
                  "%s | psram free %u min %u largest %u of %u KB | internal free %u min %u largest %u of %u KB",
                  tag, kb(psram.total_free_bytes), kb(psram.minimum_free_bytes), kb(psram.largest_free_block),
                  kb(psram.total_free_bytes + psram.total_allocated_bytes), kb(internal.total_free_bytes),
                  kb(internal.minimum_free_bytes), kb(internal.largest_free_block),
                  kb(internal.total_free_bytes + internal.total_allocated_bytes));
}

/* ---------------------------------------------------------------- 各任务 CPU 占用 */

typedef struct {
    TaskHandle_t handle;
    uint32_t run_time;
} cpu_mark_t;

static cpu_mark_t s_cpu_prev[CPU_MAX_TASKS];
static size_t s_cpu_prev_count;
static uint32_t s_cpu_total_prev;

typedef struct {
    const char *name;
    BaseType_t core;
    UBaseType_t priority;
    float percent;
} cpu_row_t;

static int compare_rows(const void *a, const void *b)
{
    const float d = ((const cpu_row_t *)b)->percent - ((const cpu_row_t *)a)->percent;
    return d > 0 ? 1 : (d < 0 ? -1 : 0);
}

/** 起点以来该任务累计运行时间的增量；新出现的任务按全部累计算 */
static uint32_t delta_run_time(TaskHandle_t handle, uint32_t now)
{
    for (size_t i = 0; i < s_cpu_prev_count; ++i) {
        if (s_cpu_prev[i].handle == handle) {
            return now - s_cpu_prev[i].run_time;
        }
    }
    return now;
}

static size_t snapshot(TaskStatus_t *status, size_t capacity, uint32_t *ret_total)
{
    return (size_t)uxTaskGetSystemState(status, (UBaseType_t)capacity, ret_total);
}

static void remember(const TaskStatus_t *status, size_t count, uint32_t total)
{
    s_cpu_prev_count = count < CPU_MAX_TASKS ? count : CPU_MAX_TASKS;
    for (size_t i = 0; i < s_cpu_prev_count; ++i) {
        s_cpu_prev[i] = (cpu_mark_t){ .handle = status[i].xHandle, .run_time = status[i].ulRunTimeCounter };
    }
    s_cpu_total_prev = total;
}

void film_perf_cpu_reset(void)
{
    TaskStatus_t *status = malloc(sizeof(TaskStatus_t) * CPU_MAX_TASKS);
    if (!status) {
        return;
    }
    uint32_t total = 0;
    const size_t count = snapshot(status, CPU_MAX_TASKS, &total);
    remember(status, count, total);
    free(status);
}

/* 绑核信息依赖 CONFIG_FREERTOS_VTASKLIST_INCLUDE_COREID；缺省时按 IDLE0/IDLE1 名字推断 */
static int task_core(const TaskStatus_t *task)
{
#if configTASKLIST_INCLUDE_COREID
    return (int)task->xCoreID;
#else
    if (strncmp(task->pcTaskName, "IDLE", 4) == 0 && task->pcTaskName[4] >= '0' && task->pcTaskName[4] <= '9') {
        return task->pcTaskName[4] - '0';
    }
    return tskNO_AFFINITY;
#endif
}

void film_perf_cpu_report(const char *tag)
{
    TaskStatus_t *status = malloc(sizeof(TaskStatus_t) * CPU_MAX_TASKS);
    cpu_row_t *rows = malloc(sizeof(cpu_row_t) * CPU_MAX_TASKS);
    if (!status || !rows) {
        free(status);
        free(rows);
        return;
    }
    uint32_t total = 0;
    const size_t count = snapshot(status, CPU_MAX_TASKS, &total);
    const uint32_t wall = total - s_cpu_total_prev;   /* 墙钟时间：两个核各自最多占满这么多 */
    size_t n = 0;
    if (wall > 0) {
        for (size_t i = 0; i < count; ++i) {
            const float percent = 100.0f * (float)delta_run_time(status[i].xHandle, status[i].ulRunTimeCounter) /
                                  (float)wall;
            rows[n++] = (cpu_row_t){ .name = status[i].pcTaskName, .core = task_core(&status[i]),
                                     .priority = status[i].uxCurrentPriority, .percent = percent };
        }
        qsort(rows, n, sizeof(rows[0]), compare_rows);
        /* 一行放不下全部任务：先打一行总览（两个核的空闲占比），再逐个打印占用最高的任务 */
        float idle[2] = { 0, 0 };
        for (size_t i = 0; i < n; ++i) {
            if (strncmp(rows[i].name, "IDLE", 4) == 0 && rows[i].core >= 0 && rows[i].core < 2) {
                idle[rows[i].core] = rows[i].percent;
            }
        }
        FILM_PERF_LOG("cpu", "%s | window %u ms | core0 busy %.0f%% core1 busy %.0f%%", tag,
                      (unsigned)(wall / 1000), 100.0f - idle[0], 100.0f - idle[1]);
        int printed = 0;
        for (size_t i = 0; i < n && printed < CPU_TOP_TASKS; ++i) {
            if (rows[i].percent < CPU_MIN_PERCENT || strncmp(rows[i].name, "IDLE", 4) == 0) {
                continue;
            }
            /* 未绑核的任务 core 为 tskNO_AFFINITY，显示为 * */
            const char core = rows[i].core == 0 ? '0' : (rows[i].core == 1 ? '1' : '*');
            FILM_PERF_LOG("cpu", "%s |   %-12s core %c prio %2u %5.1f%%", tag, rows[i].name, core,
                          (unsigned)rows[i].priority, (double)rows[i].percent);
            ++printed;
        }
    }
    remember(status, count, total);
    free(status);
    free(rows);
}

#endif /* CONFIG_FILM_PERF_TRACE */
