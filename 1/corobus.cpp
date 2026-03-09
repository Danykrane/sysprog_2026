#include "corobus.h"

#include "libcoro.h"
#include "rlist.h"

#include <cassert>

#include <deque>
#include <vector>

struct wakeup_entry {
	struct rlist base;
	struct coro *coro;
};

struct wakeup_queue {
	struct rlist coros;
};

// создать очередь ожидания
static void wakeup_queue_create(struct wakeup_queue *queue) {
	rlist_create(&queue->coros);
}

// усыпить текущую корутину
static void wakeup_queue_suspend_this(struct wakeup_queue *queue) {
	wakeup_entry waiter{};
	waiter.coro = coro_this();
	rlist_create(&waiter.base);
	rlist_add_tail_entry(&queue->coros, &waiter, base);
	coro_suspend();
	if (!rlist_empty(&waiter.base))
		rlist_del_entry(&waiter, base);
}

// разбудить первого
static void wakeup_queue_wakeup_first(struct wakeup_queue *queue) {
	if (rlist_empty(&queue->coros))
		return;
	struct wakeup_entry *waiter =
		rlist_shift_entry(&queue->coros, struct wakeup_entry, base);
	coro_wakeup(waiter->coro);
}

// разбудить всех
static void wakeup_queue_wakeup_all(struct wakeup_queue *queue) {
	while (!rlist_empty(&queue->coros)) {
		struct wakeup_entry *waiter =
			rlist_shift_entry(&queue->coros, struct wakeup_entry, base);
		coro_wakeup(waiter->coro);
	}
}

struct coro_bus_channel {
	size_t size_limit{};
	struct wakeup_queue send_queue{};
	struct wakeup_queue recv_queue{};
	std::deque<unsigned> messages;
};

struct coro_bus {
	std::vector<struct coro_bus_channel *> channels;
	std::vector<unsigned long long> generations;
};

static enum coro_bus_error_code global_error = CORO_BUS_ERR_NONE;

// вернуть последнюю ошибку
enum coro_bus_error_code coro_bus_errno() {
	return global_error;
}

// установить ошибку
void coro_bus_errno_set(const enum coro_bus_error_code err) {
	global_error = err;
}

// проверить индекс канала
static auto channel_index_is_valid(const struct coro_bus* bus, int channel) -> bool
{
	return bus != nullptr &&
		channel >= 0 &&
		static_cast<size_t>(channel) < bus->channels.size();
}

// получить канал по дескриптору
static struct coro_bus_channel *channel_get(const struct coro_bus *bus, int channel) {
	if (!channel_index_is_valid(bus, channel))
		return nullptr;
	return bus->channels[static_cast<size_t>(channel)];
}

// получить поколение канала
static unsigned long long channel_generation_get(const struct coro_bus *bus, int channel) {
	if (!channel_index_is_valid(bus, channel))
		return 0;
	return bus->generations[static_cast<size_t>(channel)];
}

// проверить, что канал не переоткрыт
static bool channel_is_same(const struct coro_bus *bus, int channel,
	unsigned long long generation) {
	return channel_index_is_valid(bus, channel) &&
		bus->channels[static_cast<size_t>(channel)] != nullptr &&
		bus->generations[static_cast<size_t>(channel)] == generation;
}

// создать канал
static struct coro_bus_channel *channel_create(size_t size_limit) {
	auto *channel = new coro_bus_channel();
	channel->size_limit = size_limit;
	wakeup_queue_create(&channel->send_queue);
	wakeup_queue_create(&channel->recv_queue);
	return channel;
}

// удалить канал
static void channel_delete(struct coro_bus_channel *channel) {
	delete channel;
}

// создать шину
struct coro_bus *coro_bus_new(void) {
	struct coro_bus *bus = new coro_bus();
	coro_bus_errno_set(CORO_BUS_ERR_NONE);
	return bus;
}

// удалить шину
void coro_bus_delete(struct coro_bus *bus) {
	if (bus == nullptr)
		return;

	for (size_t i = 0; i < bus->channels.size(); ++i) {
		struct coro_bus_channel *channel = bus->channels[i];
		if (channel == nullptr)
			continue;

		// разбудить ожидающих перед удалением
		wakeup_queue_wakeup_all(&channel->send_queue);
		wakeup_queue_wakeup_all(&channel->recv_queue);
		channel_delete(channel);
		bus->channels[i] = nullptr;
		++bus->generations[i];
	}

	delete bus;
}

// открыть канал
int coro_bus_channel_open(struct coro_bus *bus, size_t size_limit) {
	assert(bus != nullptr);

	// переиспользовать свободный слот
	for (size_t i = 0; i < bus->channels.size(); ++i) {
		if (bus->channels[i] == nullptr) {
			bus->channels[i] = channel_create(size_limit);
			coro_bus_errno_set(CORO_BUS_ERR_NONE);
			return static_cast<int>(i);
		}
	}

	// добавить новый слот
	bus->channels.push_back(channel_create(size_limit));
	bus->generations.push_back(1);
	coro_bus_errno_set(CORO_BUS_ERR_NONE);
	return static_cast<int>(bus->channels.size() - 1);
}

// закрыть канал
void coro_bus_channel_close(struct coro_bus *bus, int channel) {
	assert(bus != nullptr);
	assert(channel_index_is_valid(bus, channel));

	struct coro_bus_channel *closing_channel = bus->channels[static_cast<size_t>(channel)];
	assert(closing_channel != nullptr);

	// убрать канал из шины
	bus->channels[static_cast<size_t>(channel)] = nullptr;
	++bus->generations[static_cast<size_t>(channel)];

	// разбудить всех ожидающих
	wakeup_queue_wakeup_all(&closing_channel->send_queue);
	wakeup_queue_wakeup_all(&closing_channel->recv_queue);

	channel_delete(closing_channel);
}

// неблокирующая отправка
int coro_bus_try_send(struct coro_bus *bus, int channel, unsigned data) {
	struct coro_bus_channel *target_channel = channel_get(bus, channel);
	if (target_channel == nullptr) {
		coro_bus_errno_set(CORO_BUS_ERR_NO_CHANNEL);
		return -1;
	}

	// канал заполнен
	if (target_channel->messages.size() >= target_channel->size_limit) {
		coro_bus_errno_set(CORO_BUS_ERR_WOULD_BLOCK);
		return -1;
	}

	// добавить сообщение
	target_channel->messages.push_back(data);
	wakeup_queue_wakeup_first(&target_channel->recv_queue);
	coro_bus_errno_set(CORO_BUS_ERR_NONE);
	return 0;
}

// блокирующая отправка
int coro_bus_send(struct coro_bus *bus, int channel, unsigned data) {
	for (;;) {
		int rc = coro_bus_try_send(bus, channel, data);
		if (rc == 0)
			return 0;
		if (coro_bus_errno() == CORO_BUS_ERR_NO_CHANNEL)
			return -1;

		struct coro_bus_channel *target_channel = channel_get(bus, channel);
		unsigned long long generation = channel_generation_get(bus, channel);
		if (target_channel == nullptr) {
			coro_bus_errno_set(CORO_BUS_ERR_NO_CHANNEL);
			return -1;
		}

		// ждать место
		wakeup_queue_suspend_this(&target_channel->send_queue);

		// канал могли закрыть
		if (!channel_is_same(bus, channel, generation)) {
			coro_bus_errno_set(CORO_BUS_ERR_NO_CHANNEL);
			return -1;
		}
	}
}

// неблокирующее получение
int coro_bus_try_recv(struct coro_bus *bus, int channel, unsigned *data) {
	struct coro_bus_channel *target_channel = channel_get(bus, channel);
	if (target_channel == nullptr) {
		coro_bus_errno_set(CORO_BUS_ERR_NO_CHANNEL);
		return -1;
	}

	// канал пуст
	if (target_channel->messages.empty()) {
		coro_bus_errno_set(CORO_BUS_ERR_WOULD_BLOCK);
		return -1;
	}

	// забрать сообщение
	*data = target_channel->messages.front();
	target_channel->messages.pop_front();
	wakeup_queue_wakeup_first(&target_channel->send_queue);
	coro_bus_errno_set(CORO_BUS_ERR_NONE);
	return 0;
}

// блокирующее получение
int coro_bus_recv(struct coro_bus *bus, int channel, unsigned *data) {
	for (;;) {
		int rc = coro_bus_try_recv(bus, channel, data);
		if (rc == 0)
			return 0;
		if (coro_bus_errno() == CORO_BUS_ERR_NO_CHANNEL)
			return -1;

		struct coro_bus_channel *target_channel = channel_get(bus, channel);
		unsigned long long generation = channel_generation_get(bus, channel);
		if (target_channel == nullptr) {
			coro_bus_errno_set(CORO_BUS_ERR_NO_CHANNEL);
			return -1;
		}

		// ждать сообщение
		wakeup_queue_suspend_this(&target_channel->recv_queue);

		// канал могли закрыть
		if (!channel_is_same(bus, channel, generation)) {
			coro_bus_errno_set(CORO_BUS_ERR_NO_CHANNEL);
			return -1;
		}
	}
}

#if NEED_BROADCAST

// блокирующая рассылка
int coro_bus_broadcast(struct coro_bus *bus, unsigned data) {
	for (;;) {
		int rc = coro_bus_try_broadcast(bus, data);
		if (rc == 0)
			return 0;
		if (coro_bus_errno() == CORO_BUS_ERR_NO_CHANNEL)
			return -1;

		struct coro_bus_channel *blocking_channel = nullptr;

		// найти первый полный канал
		for (auto channel : bus->channels) {
			if (channel != nullptr &&
				channel->messages.size() >= channel->size_limit) {
				blocking_channel = channel;
				break;
			}
		}

		if (blocking_channel == nullptr)
			continue;

		// ждать место
		wakeup_queue_suspend_this(&blocking_channel->send_queue);
	}
}

// неблокирующая рассылка
int coro_bus_try_broadcast(struct coro_bus *bus, unsigned data) {
	if (bus == nullptr) {
		coro_bus_errno_set(CORO_BUS_ERR_NO_CHANNEL);
		return -1;
	}

	bool has_channels = false;

	// проверить, что все каналы готовы
	for (auto channel : bus->channels) {
			if (channel == nullptr)
			continue;

		has_channels = true;
		if (channel->messages.size() >= channel->size_limit) {
			coro_bus_errno_set(CORO_BUS_ERR_WOULD_BLOCK);
			return -1;
		}
	}

	if (!has_channels) {
		coro_bus_errno_set(CORO_BUS_ERR_NO_CHANNEL);
		return -1;
	}

	// отправить во все каналы
	for (auto channel : bus->channels) {
			if (channel == nullptr)
			continue;

		channel->messages.push_back(data);
		wakeup_queue_wakeup_first(&channel->recv_queue);
	}

	coro_bus_errno_set(CORO_BUS_ERR_NONE);
	return 0;
}

#endif

#if NEED_BATCH

// неблокирующая отправка пачки
int coro_bus_try_send_v(struct coro_bus *bus, int channel, const unsigned *data, unsigned count) {
	struct coro_bus_channel *target_channel = channel_get(bus, channel);
	if (target_channel == nullptr) {
		coro_bus_errno_set(CORO_BUS_ERR_NO_CHANNEL);
		return -1;
	}

	if (count == 0) {
		coro_bus_errno_set(CORO_BUS_ERR_NONE);
		return 0;
	}

	size_t free_space = target_channel->size_limit - target_channel->messages.size();
	if (free_space == 0) {
		coro_bus_errno_set(CORO_BUS_ERR_WOULD_BLOCK);
		return -1;
	}

	unsigned send_count = count;
	if (static_cast<size_t>(send_count) > free_space)
		send_count = static_cast<unsigned>(free_space);

	// отправить сколько помещается
	for (unsigned i = 0; i < send_count; ++i)
		target_channel->messages.push_back(data[i]);

	wakeup_queue_wakeup_first(&target_channel->recv_queue);
	coro_bus_errno_set(CORO_BUS_ERR_NONE);
	return static_cast<int>(send_count);
}

// блокирующая отправка пачки
int coro_bus_send_v(struct coro_bus *bus, int channel, const unsigned *data, unsigned count) {
	if (count == 0) {
		coro_bus_errno_set(CORO_BUS_ERR_NONE);
		return 0;
	}

	for (;;) {
		int rc = coro_bus_try_send_v(bus, channel, data, count);
		if (rc >= 0)
			return rc;
		if (coro_bus_errno() == CORO_BUS_ERR_NO_CHANNEL)
			return -1;

		struct coro_bus_channel *target_channel = channel_get(bus, channel);
		unsigned long long generation = channel_generation_get(bus, channel);
		if (target_channel == nullptr) {
			coro_bus_errno_set(CORO_BUS_ERR_NO_CHANNEL);
			return -1;
		}

		wakeup_queue_suspend_this(&target_channel->send_queue);

		if (!channel_is_same(bus, channel, generation)) {
			coro_bus_errno_set(CORO_BUS_ERR_NO_CHANNEL);
			return -1;
		}
	}
}

// неблокирующее получение пачки
int coro_bus_try_recv_v(struct coro_bus *bus, int channel, unsigned *data, unsigned capacity) {
	struct coro_bus_channel *target_channel = channel_get(bus, channel);
	if (target_channel == nullptr) {
		coro_bus_errno_set(CORO_BUS_ERR_NO_CHANNEL);
		return -1;
	}

	if (capacity == 0) {
		coro_bus_errno_set(CORO_BUS_ERR_NONE);
		return 0;
	}

	if (target_channel->messages.empty()) {
		coro_bus_errno_set(CORO_BUS_ERR_WOULD_BLOCK);
		return -1;
	}

	unsigned recv_count = capacity;
	if (static_cast<size_t>(recv_count) > target_channel->messages.size())
		recv_count = static_cast<unsigned>(target_channel->messages.size());

	// забрать сколько есть
	for (unsigned i = 0; i < recv_count; ++i) {
		data[i] = target_channel->messages.front();
		target_channel->messages.pop_front();
	}

	wakeup_queue_wakeup_first(&target_channel->send_queue);
	coro_bus_errno_set(CORO_BUS_ERR_NONE);
	return static_cast<int>(recv_count);
}

// блокирующее получение пачки
int coro_bus_recv_v(struct coro_bus *bus, int channel, unsigned *data, unsigned capacity) {
	if (capacity == 0) {
		coro_bus_errno_set(CORO_BUS_ERR_NONE);
		return 0;
	}

	for (;;) {
		int rc = coro_bus_try_recv_v(bus, channel, data, capacity);
		if (rc >= 0)
			return rc;
		if (coro_bus_errno() == CORO_BUS_ERR_NO_CHANNEL)
			return -1;

		struct coro_bus_channel *target_channel = channel_get(bus, channel);
		unsigned long long generation = channel_generation_get(bus, channel);
		if (target_channel == nullptr) {
			coro_bus_errno_set(CORO_BUS_ERR_NO_CHANNEL);
			return -1;
		}

		wakeup_queue_suspend_this(&target_channel->recv_queue);

		if (!channel_is_same(bus, channel, generation)) {
			coro_bus_errno_set(CORO_BUS_ERR_NO_CHANNEL);
			return -1;
		}
	}
}

#endif