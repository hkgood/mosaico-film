/*
 * film_worker 的 pthread 实现，仅用于主机测试。
 * 状态都在 lock 保护下：busy 表示有待执行/执行中的作业，quit 表示要退出。
 */
#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>

#include "film_worker.h"

struct film_worker_t {
    pthread_t thread;
    pthread_mutex_t lock;
    pthread_cond_t cond;
    film_worker_job_fn_t job;
    void *arg;
    bool busy;
    bool quit;
};

static void *worker_main(void *param)
{
    film_worker_t *worker = param;
    pthread_mutex_lock(&worker->lock);
    for (;;) {
        while (!worker->busy && !worker->quit) {
            pthread_cond_wait(&worker->cond, &worker->lock);
        }
        if (worker->quit) {
            break;
        }
        pthread_mutex_unlock(&worker->lock);
        worker->job(worker->arg);
        pthread_mutex_lock(&worker->lock);
        worker->busy = false;
        pthread_cond_broadcast(&worker->cond);
    }
    pthread_mutex_unlock(&worker->lock);
    return NULL;
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
    if (pthread_mutex_init(&worker->lock, NULL) != 0) {
        free(worker);
        return ESP_ERR_NO_MEM;
    }
    if (pthread_cond_init(&worker->cond, NULL) != 0) {
        pthread_mutex_destroy(&worker->lock);
        free(worker);
        return ESP_ERR_NO_MEM;
    }
    if (pthread_create(&worker->thread, NULL, worker_main, worker) != 0) {
        pthread_cond_destroy(&worker->cond);
        pthread_mutex_destroy(&worker->lock);
        free(worker);
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
    pthread_mutex_lock(&worker->lock);
    worker->quit = true;
    pthread_cond_broadcast(&worker->cond);
    pthread_mutex_unlock(&worker->lock);
    pthread_join(worker->thread, NULL);
    pthread_cond_destroy(&worker->cond);
    pthread_mutex_destroy(&worker->lock);
    free(worker);
}

void film_worker_start(film_worker_t *worker, film_worker_job_fn_t job, void *arg)
{
    pthread_mutex_lock(&worker->lock);
    worker->job = job;
    worker->arg = arg;
    worker->busy = true;
    pthread_cond_broadcast(&worker->cond);
    pthread_mutex_unlock(&worker->lock);
}

void film_worker_wait(film_worker_t *worker)
{
    pthread_mutex_lock(&worker->lock);
    while (worker->busy) {
        pthread_cond_wait(&worker->cond, &worker->lock);
    }
    pthread_mutex_unlock(&worker->lock);
}
