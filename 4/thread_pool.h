#pragma once

#include <functional>


#define NEED_DETACH 1
#define NEED_TIMED_JOIN 1

struct thread_pool;
struct thread_task;

using thread_task_f = std::function<void(void)>;

enum {
	TPOOL_MAX_THREADS = 20,
	TPOOL_MAX_TASKS = 100000,
};

enum thread_pool_errcode {
	TPOOL_ERR_INVALID_ARGUMENT = 1,
	TPOOL_ERR_TOO_MANY_TASKS,
	TPOOL_ERR_HAS_TASKS,
	TPOOL_ERR_TASK_NOT_PUSHED,
	TPOOL_ERR_TASK_IN_POOL,
	TPOOL_ERR_NOT_IMPLEMENTED,
	TPOOL_ERR_TIMEOUT,
};

// создать пул потоков
int thread_pool_new(int thread_count, thread_pool **pool);

// удалить пул
int thread_pool_delete(thread_pool *pool);

// добавить задачу в пул
int thread_pool_push_task(thread_pool *pool, thread_task *task);

// создать задачу
int thread_task_new(thread_task **task, const thread_task_f &function);

// проверить, что задача завершена и уже joined
bool thread_task_is_finished(const thread_task *task);

// проверить, что задача сейчас выполняется
bool thread_task_is_running(const thread_task *task);

// дождаться завершения задачи
int thread_task_join(thread_task *task);

#if NEED_TIMED_JOIN
// дождаться завершения задачи с таймаутом
int thread_task_timed_join(thread_task *task, double timeout);
#endif

// удалить задачу
int thread_task_delete(const thread_task *task);

#if NEED_DETACH
// отвязать задачу с автоудалением
int thread_task_detach(thread_task *task);
#endif