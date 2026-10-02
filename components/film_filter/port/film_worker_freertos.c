/*
 * film_worker 的 FreeRTOS 实现：工作任务阻塞在 start 信号量上，
 * 每收到一次信号执行一个作业，完成后释放 done 信号量。
 */
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "film_worker.h"

#define FILM_WORKER_NAME        "film_worker"
/* 作业只是逐行循环，最深的是像素风的浮点量化，栈用量很小 */
#define FILM_WORKER_STACK_BYTES 3072
#define FILM_WORKER_IDLE_POLL_TICKS 1

struct film_worker_t {
    TaskHandle_t task;
    SemaphoreHandle_t start;
    SemaphoreHandle_t done;
    film_worker_job_fn_t job;   /*!< 由所有者在 give(start) 前写入，工作任务在 take(start) 后读取 */
    void *arg;
};

static void worker_main(void *param)
{
    film_worker_t *worker = param;
    for (;;) {
        xSemaphoreTake(worker->start, portMAX_DELAY);
        worker->job(worker->arg);
        xSemaphoreGive(worker->done);
    }
}

esp_err_t film_worker_create(film_worker_t **ret_worker)
{
    if (ret_worker == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    film_worker_t *worker = calloc(1, sizeof(*worker));
    if (worker == NULL) {
        return ESP_ERR_NO_MEM;
    }
    worker->start = xSemaphoreCreateBinary();
    worker->done = xSemaphoreCreateBinary();
    if (worker->start == NULL || worker->done == NULL) {
        film_worker_delete(worker);
        return ESP_ERR_NO_MEM;
    }

#if CONFIG_FREERTOS_NUMBER_OF_CORES > 1
    BaseType_t core = (BaseType_t)(xPortGetCoreID() == 0 ? 1 : 0);
#else
    BaseType_t core = tskNO_AFFINITY;
#endif
    if (xTaskCreatePinnedToCore(worker_main, FILM_WORKER_NAME, FILM_WORKER_STACK_BYTES, worker,
                                uxTaskPriorityGet(NULL), &worker->task, core) != pdPASS) {
        worker->task = NULL;
        film_worker_delete(worker);
        return ESP_ERR_NO_MEM;
    }
    *ret_worker = worker;
    return ESP_OK;
}

void film_worker_delete(film_worker_t *worker)
{
    if (worker == NULL) {
        return;
    }
    if (worker->task != NULL) {
        /* 所有作业都已 wait 过，工作任务要么已停在 start 上，要么正从 give(done) 走回去；
         * 等它真正阻塞后再删除，保证它不再访问信号量和本结构 */
        while (eTaskGetState(worker->task) != eBlocked) {
            vTaskDelay(FILM_WORKER_IDLE_POLL_TICKS);
        }
        vTaskDelete(worker->task);
    }
    if (worker->start != NULL) {
        vSemaphoreDelete(worker->start);
    }
    if (worker->done != NULL) {
        vSemaphoreDelete(worker->done);
    }
    free(worker);
}

void film_worker_start(film_worker_t *worker, film_worker_job_fn_t job, void *arg)
{
    worker->job = job;
    worker->arg = arg;
    xSemaphoreGive(worker->start);
}

void film_worker_wait(film_worker_t *worker)
{
    xSemaphoreTake(worker->done, portMAX_DELAY);
}
