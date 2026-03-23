#include "thread_pool.h"

#include <pthread.h>

#include <cmath>
#include <ctime>
#include <deque>
#include <cerrno>
#include <utility>
#include <vector>

namespace {

constexpr int SUCCESS = 0;
constexpr long NSEC_PER_SEC = 1000000000L;

#if defined(__linux__)
constexpr clockid_t condition_clock = CLOCK_MONOTONIC;
#else
constexpr clockid_t condition_clock = CLOCK_REALTIME;
#endif

enum task_state {
	TASK_NEW = 0,
	TASK_QUEUED,
	TASK_RUNNING,
	TASK_FINISHED,
};

// инициализировать condvar
void cond_init(pthread_cond_t *cond) {
#if defined(__linux__)
	pthread_condattr_t attr;
	if (pthread_condattr_init(&attr) == 0) {
		if (pthread_condattr_setclock(&attr, condition_clock) == 0) {
			if (pthread_cond_init(cond, &attr) == 0) {
				pthread_condattr_destroy(&attr);
				return;
			}
		}
		pthread_condattr_destroy(&attr);
	}
#endif
	pthread_cond_init(cond, nullptr);
}

// посчитать абсолютный дедлайн для timed wait
void make_deadline(timespec *deadline, double timeout) {
	clock_gettime(condition_clock, deadline);

	if (timeout <= 0.0)
		return;

	if (!std::isfinite(timeout) || timeout > 1000000000.0) {
		deadline->tv_sec += 1000000000L;
		return;
	}

	const double seconds_part = std::floor(timeout);
	const double nanoseconds_part =
		(timeout - seconds_part) * static_cast<double>(NSEC_PER_SEC);

	deadline->tv_sec += static_cast<time_t>(seconds_part);
	deadline->tv_nsec += static_cast<long>(nanoseconds_part);

	if (deadline->tv_nsec >= NSEC_PER_SEC) {
		deadline->tv_sec += deadline->tv_nsec / NSEC_PER_SEC;
		deadline->tv_nsec %= NSEC_PER_SEC;
	}
}

} // namespace

struct thread_task {
	explicit thread_task(thread_task_f task_function)
		: function(std::move(task_function)) {
		pthread_mutex_init(&mutex, nullptr);
		cond_init(&finished_cond);
	}

	~thread_task() {
		pthread_cond_destroy(&finished_cond);
		pthread_mutex_destroy(&mutex);
	}

	thread_task_f function;
	mutable pthread_mutex_t mutex {};
	pthread_cond_t finished_cond {};

	thread_pool *owner_pool = nullptr;
	task_state state = TASK_NEW;
	bool joined = false;
	bool detached = false;
};

struct thread_pool {
	explicit thread_pool(int max_thread_count)
		: max_threads(max_thread_count) {
		pthread_mutex_init(&mutex, nullptr);
		cond_init(&queue_cond);
	}

	~thread_pool() {
		pthread_cond_destroy(&queue_cond);
		pthread_mutex_destroy(&mutex);
	}

	// уже созданные worker-потоки
	std::vector<pthread_t> threads;
	// очередь ожидающих задач
	std::deque<thread_task*> queue;

	// общий mutex пула
	pthread_mutex_t mutex {};
	// condvar для пробуждения worker-ов
	pthread_cond_t queue_cond {};

	// верхний лимит потоков
	int max_threads = 0;
	// сколько потоков сейчас спят в ожидании задачи
	int idle_threads = 0;
	// число задач, которые считаются принадлежащими пулу
	size_t task_count = 0;
	// признак остановки пула
	bool stop = false;
};

// задача еще ни разу не отправлялась в пул
bool task_is_not_pushed_locked(const thread_task *task) {
	return task->owner_pool == nullptr && task->state == TASK_NEW;
}

// задача уже завершена и полностью освобождена от пула
bool task_is_released_locked(const thread_task *task) {
	return task->state == TASK_FINISHED &&
		task->joined &&
		task->owner_pool == nullptr;
}

// задачу нельзя повторно пушить
bool task_is_in_pool_locked(const thread_task *task) {
	return task->owner_pool != nullptr ||
		task->state == TASK_QUEUED ||
		task->state == TASK_RUNNING ||
		(task->state == TASK_FINISHED && !task->joined);
}

// уменьшить число задач в пуле
void pool_decrease_task_count(thread_pool *pool) {
	if (pool == nullptr)
		return;

	pthread_mutex_lock(&pool->mutex);
	if (pool->task_count > 0)
		--pool->task_count;
	pthread_mutex_unlock(&pool->mutex);
}

// worker loop
void *worker_main(void *arg) {
	auto *pool = static_cast<thread_pool*>(arg);

	while (true) {
		pthread_mutex_lock(&pool->mutex);

		// спать, пока нет задач и пул не остановлен
		while (!pool->stop && pool->queue.empty()) {
			++pool->idle_threads;
			pthread_cond_wait(&pool->queue_cond, &pool->mutex);
			--pool->idle_threads;
		}

		// если остановка и задач больше нет — выйти
		if (pool->stop && pool->queue.empty()) {
			pthread_mutex_unlock(&pool->mutex);
			return nullptr;
		}

		// взять задачу из очереди
		thread_task *task = pool->queue.front();
		pool->queue.pop_front();
		pthread_mutex_unlock(&pool->mutex);

		// перевести задачу в состояние RUNNING
		pthread_mutex_lock(&task->mutex);
		task->state = TASK_RUNNING;
		pthread_mutex_unlock(&task->mutex);

		// выполнить пользовательскую функцию
		try {
			task->function();
		} catch (...) {
			// исключения наружу не выпускаем
		}

		// после выполнения нужно либо просто завершить задачу,
		// либо удалить ее, если она была detach
		bool should_delete_task = false;
		thread_pool *owner_pool = nullptr;

		pthread_mutex_lock(&task->mutex);
		task->state = TASK_FINISHED;

		if (task->detached) {
			task->joined = true;
			owner_pool = task->owner_pool;
			task->owner_pool = nullptr;
			should_delete_task = true;
		}

		// разбудить всех, кто ждет завершения
		pthread_cond_broadcast(&task->finished_cond);
		pthread_mutex_unlock(&task->mutex);

		// detach-задача удаляется автоматически после завершения
		if (should_delete_task) {
			pool_decrease_task_count(owner_pool);
			delete task;
		}
	}
}

// создать новый worker по требованию
bool start_worker(thread_pool *pool) {
	pthread_t thread_id {};
	const int rc = pthread_create(&thread_id, nullptr, worker_main, pool);
	if (rc != 0)
		return false;

	pool->threads.push_back(thread_id);
	return true;
}

int thread_pool_new(int thread_count, thread_pool **pool) {
	if (pool == nullptr)
		return TPOOL_ERR_INVALID_ARGUMENT;
	if (thread_count <= 0 || thread_count > TPOOL_MAX_THREADS) {
		*pool = nullptr;
		return TPOOL_ERR_INVALID_ARGUMENT;
	}

	*pool = new thread_pool(thread_count);
	return SUCCESS;
}

int thread_pool_delete(thread_pool *pool) {
	if (pool == nullptr)
		return TPOOL_ERR_INVALID_ARGUMENT;

	pthread_mutex_lock(&pool->mutex);

	// пул нельзя удалять, пока в нем есть задачи
	if (pool->task_count != 0) {
		pthread_mutex_unlock(&pool->mutex);
		return TPOOL_ERR_HAS_TASKS;
	}

	// сообщить worker-ам, что пора завершаться
	pool->stop = true;
	pthread_cond_broadcast(&pool->queue_cond);
	pthread_mutex_unlock(&pool->mutex);

	// дождаться завершения всех worker-ов
	for (pthread_t thread_id : pool->threads)
		pthread_join(thread_id, nullptr);

	delete pool;
	return SUCCESS;
}

int thread_pool_push_task(thread_pool *pool, thread_task *task) {
	if (pool == nullptr || task == nullptr)
		return TPOOL_ERR_INVALID_ARGUMENT;

	pthread_mutex_lock(&pool->mutex);

	// лимит по числу задач
	if (pool->task_count >= TPOOL_MAX_TASKS) {
		pthread_mutex_unlock(&pool->mutex);
		return TPOOL_ERR_TOO_MANY_TASKS;
	}

	pthread_mutex_lock(&task->mutex);

	// нельзя повторно отправить задачу, которая еще принадлежит пулу
	if (task_is_in_pool_locked(task)) {
		pthread_mutex_unlock(&task->mutex);
		pthread_mutex_unlock(&pool->mutex);
		return TPOOL_ERR_TASK_IN_POOL;
	}

	// подготовить задачу к новому запуску
	task->owner_pool = pool;
	task->state = TASK_QUEUED;
	task->joined = false;
	task->detached = false;

	pthread_mutex_unlock(&task->mutex);

	// добавить задачу в очередь
	pool->queue.push_back(task);
	++pool->task_count;

	// если никто не спит и можно создать новый поток — создаем
	if (pool->idle_threads == 0 &&
		static_cast<int>(pool->threads.size()) < pool->max_threads) {
		if (!start_worker(pool)) {
			// откатить изменения при ошибке создания потока
			pool->queue.pop_back();
			--pool->task_count;

			pthread_mutex_lock(&task->mutex);
			task->owner_pool = nullptr;
			task->state = TASK_NEW;
			task->joined = false;
			task->detached = false;
			pthread_mutex_unlock(&task->mutex);

			pthread_mutex_unlock(&pool->mutex);
			return TPOOL_ERR_INVALID_ARGUMENT;
		}
	}

	// разбудить один спящий worker
	pthread_cond_signal(&pool->queue_cond);
	pthread_mutex_unlock(&pool->mutex);
	return SUCCESS;
}

int thread_task_new(thread_task **task, const thread_task_f &function) {
	if (task == nullptr)
		return TPOOL_ERR_INVALID_ARGUMENT;

	*task = new thread_task(function);
	return SUCCESS;
}

bool thread_task_is_finished(const thread_task *task) {
	if (task == nullptr)
		return false;

	pthread_mutex_lock(&task->mutex);
	const bool is_finished = task_is_released_locked(task);
	pthread_mutex_unlock(&task->mutex);
	return is_finished;
}

bool thread_task_is_running(const thread_task *task) {
	if (task == nullptr)
		return false;

	pthread_mutex_lock(&task->mutex);
	const bool is_running = task->state == TASK_RUNNING;
	pthread_mutex_unlock(&task->mutex);
	return is_running;
}

int thread_task_join(thread_task *task) {
	if (task == nullptr)
		return TPOOL_ERR_INVALID_ARGUMENT;

	pthread_mutex_lock(&task->mutex);

	// join для задачи, которая никогда не пушилась
	if (task_is_not_pushed_locked(task)) {
		pthread_mutex_unlock(&task->mutex);
		return TPOOL_ERR_TASK_NOT_PUSHED;
	}

	// повторный join уже завершенной и освобожденной задачи
	if (task_is_released_locked(task)) {
		pthread_mutex_unlock(&task->mutex);
		return SUCCESS;
	}

	// ждать завершения задачи
	while (task->state != TASK_FINISHED)
		pthread_cond_wait(&task->finished_cond, &task->mutex);

	// после пробуждения задача уже могла быть оформлена как joined
	if (task_is_released_locked(task)) {
		pthread_mutex_unlock(&task->mutex);
		return SUCCESS;
	}

	// зафиксировать результат join
	thread_pool *owner_pool = task->owner_pool;
	task->joined = true;
	task->owner_pool = nullptr;
	pthread_mutex_unlock(&task->mutex);

	pool_decrease_task_count(owner_pool);
	return SUCCESS;
}

#if NEED_TIMED_JOIN
int thread_task_timed_join(thread_task *task, double timeout) {
	if (task == nullptr)
		return TPOOL_ERR_INVALID_ARGUMENT;

	pthread_mutex_lock(&task->mutex);

	// timed join для задачи, которая не пушилась
	if (task_is_not_pushed_locked(task)) {
		pthread_mutex_unlock(&task->mutex);
		return TPOOL_ERR_TASK_NOT_PUSHED;
	}

	// повторный timed join по уже завершенной joined-задаче
	if (task_is_released_locked(task)) {
		pthread_mutex_unlock(&task->mutex);
		return SUCCESS;
	}

	// если задача еще не закончена, ждем не дольше timeout
	if (task->state != TASK_FINISHED) {
		if (timeout <= 0.0) {
			pthread_mutex_unlock(&task->mutex);
			return TPOOL_ERR_TIMEOUT;
		}

		timespec deadline {};
		make_deadline(&deadline, timeout);

		while (task->state != TASK_FINISHED) {
			const int rc =
				pthread_cond_timedwait(&task->finished_cond, &task->mutex, &deadline);

			if (rc == ETIMEDOUT && task->state != TASK_FINISHED) {
				pthread_mutex_unlock(&task->mutex);
				return TPOOL_ERR_TIMEOUT;
			}
		}
	}

	// если кто-то уже успел сделать обычный join
	if (task_is_released_locked(task)) {
		pthread_mutex_unlock(&task->mutex);
		return SUCCESS;
	}

	// завершить timed join как обычный join
	thread_pool *owner_pool = task->owner_pool;
	task->joined = true;
	task->owner_pool = nullptr;
	pthread_mutex_unlock(&task->mutex);

	pool_decrease_task_count(owner_pool);
	return SUCCESS;
}
#endif

int thread_task_delete(const thread_task *task) {
	if (task == nullptr)
		return TPOOL_ERR_INVALID_ARGUMENT;

	pthread_mutex_lock(&task->mutex);

	// удалять можно только задачу, которая уже не принадлежит пулу
	if (task_is_in_pool_locked(task)) {
		pthread_mutex_unlock(&task->mutex);
		return TPOOL_ERR_TASK_IN_POOL;
	}

	pthread_mutex_unlock(&task->mutex);
	delete task;
	return SUCCESS;
}

#if NEED_DETACH
int thread_task_detach(thread_task *task) {
	if (task == nullptr)
		return TPOOL_ERR_INVALID_ARGUMENT;

	pthread_mutex_lock(&task->mutex);

	// detach для задачи, которая еще не отправлялась в пул
	if (task_is_not_pushed_locked(task)) {
		pthread_mutex_unlock(&task->mutex);
		return TPOOL_ERR_TASK_NOT_PUSHED;
	}

	// если задача уже завершена, но еще числится за пулом,
	// ее можно удалить прямо сейчас
	if (task->state == TASK_FINISHED && task->owner_pool != nullptr) {
		thread_pool *owner_pool = task->owner_pool;
		task->detached = true;
		task->joined = true;
		task->owner_pool = nullptr;
		pthread_mutex_unlock(&task->mutex);

		pool_decrease_task_count(owner_pool);
		delete task;
		return SUCCESS;
	}

	// если задача еще выполняется или стоит в очереди,
	// worker удалит ее сам после завершения
	task->detached = true;
	pthread_mutex_unlock(&task->mutex);
	return SUCCESS;
}
#endif