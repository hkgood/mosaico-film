/*
 * 滤镜内部的"第二只手"：一个常驻的工作任务，一次只执行一个作业。
 *
 * 平台相关部分只有这一处：设备上用 FreeRTOS 任务（port/film_worker_freertos.c），
 * 主机测试用 pthread（port/film_worker_pthread.c）。
 *
 * 所有权：工作任务归创建它的 film_filter 实例所有。start / wait / delete 只能由
 * 该实例的所有者任务调用，并且每次 start 之后必须先 wait 再发起下一次 start 或 delete。
 * 作业参数在 wait 返回前必须一直有效（调用者栈上的变量即可）。
 */
#pragma once

#include "esp_err.h"

typedef struct film_worker_t film_worker_t;

typedef void (*film_worker_job_fn_t)(void *arg);

/**
 * 创建工作任务。设备上会把它固定到调用者当前所在核之外的另一个核，
 * 优先级与调用者相同；因此建议在固定了核的任务里创建并使用滤镜。
 */
esp_err_t film_worker_create(film_worker_t **ret_worker);

/** 停止并释放工作任务；必须在没有未完成作业时调用 */
void film_worker_delete(film_worker_t *worker);

/** 让工作任务异步执行 job(arg) */
void film_worker_start(film_worker_t *worker, film_worker_job_fn_t job, void *arg);

/** 等待上一次 start 的作业完成 */
void film_worker_wait(film_worker_t *worker);
