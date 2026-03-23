#include "chat_client.h"

#include <arpa/inet.h>
#include <cerrno>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <climits>
#include <cstring>
#include <cmath>
#include <deque>
#include <memory>
#include <string>
#include <string_view>

#include "chat.h"

struct chat_client {
	int socket = -1;

#if NEED_AUTHOR
	std::string name;
	bool name_sent = false;
#endif

	// очередь принятых сообщений
	std::deque<chat_message *> incoming;

	// буфер текста до '\n'
	std::string feed_buffer;

	// буфер отправки в сокет
	std::string out_buffer;
	size_t out_offset = 0;

	// разбор входящих фреймов
	frame_parser input;
};

// дозаписать буфер в сокет
static bool clientFlush(chat_client *client) {
	while (client->socket >= 0 && client->out_offset < client->out_buffer.size()) {
		const size_t left = client->out_buffer.size() - client->out_offset;
		const ssize_t value = send(
			client->socket,
			client->out_buffer.data() + client->out_offset,
			left,
			MSG_NOSIGNAL);

		if (value > 0) {
			client->out_offset += static_cast<size_t>(value);
			continue;
		}
		if (value == 0)
			break;
		if (errno == EINTR)
			continue;
		if (errno == EAGAIN || errno == EWOULDBLOCK)
			return true;
		return false;
	}

	if (client->out_offset >= client->out_buffer.size()) {
		client->out_buffer.clear();
		client->out_offset = 0;
	}
	return true;
}

// считать все доступные данные из сокета
bool clientRead(chat_client *client) {
	char temporary[64 * 1024];

	while (true) {
		const ssize_t value = recv(client->socket, temporary, sizeof(temporary), 0);

		if (value > 0) {
			client->input.buffer.append(temporary, static_cast<size_t>(value));

			std::string parsed_author;
			std::string parsed_data;
			while (client->input.try_pop(parsed_author, parsed_data)) {
				if (parsed_data.empty())
					continue;

				auto message = std::make_unique<chat_message>();
#if NEED_AUTHOR
				message->author = std::move(parsed_author);
#endif
				message->data = std::move(parsed_data);

				client->incoming.push_back(message.get());
				(void)message.release();

				parsed_author.clear();
				parsed_data.clear();
			}
			continue;
		}

		if (value == 0)
			return false;
		if (errno == EINTR)
			continue;
		if (errno == EAGAIN || errno == EWOULDBLOCK)
			break;
		return false;
	}

	return true;
}

chat_client *chat_client_new(const std::string_view name) {
	auto *client = new chat_client();
#if NEED_AUTHOR
	client->name.assign(name.data(), name.size());
#else
	(void)name;
#endif
	return client;
}

void chat_client_delete(chat_client *client) {
	if (client == nullptr)
		return;

	if (client->socket >= 0)
		close(client->socket);

	while (!client->incoming.empty()) {
		delete client->incoming.front();
		client->incoming.pop_front();
	}

	delete client;
}

int chat_client_connect(chat_client *client, const std::string_view address) {
	if (client == nullptr)
		return CHAT_ERR_INVALID_ARGUMENT;
	if (client->socket >= 0)
		return CHAT_ERR_ALREADY_STARTED;

	std::string host;
	std::string port;
	int result = parseAddress(address, host, port);
	if (result != 0)
		return result;

	addrinfo hints {};
	std::memset(&hints, 0, sizeof(hints));
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_family = AF_INET;

	addrinfo *address_result = nullptr;
	result = getaddrinfo(host.c_str(), port.c_str(), &hints, &address_result);
	if (result != 0)
		return CHAT_ERR_NO_ADDR;

	int file_descriptor = -1;
	for (const addrinfo *it = address_result; it != nullptr; it = it->ai_next) {
		file_descriptor = socket(it->ai_family, it->ai_socktype, it->ai_protocol);
		if (file_descriptor < 0)
			continue;

		if (connect(file_descriptor, it->ai_addr, it->ai_addrlen) == 0)
			break;

		close(file_descriptor);
		file_descriptor = -1;
	}
	freeaddrinfo(address_result);

	if (file_descriptor < 0)
		return CHAT_ERR_SYS;

	// после connect сокет должен быть неблокирующим
	if (setNonBlocking(file_descriptor) != 0) {
		close(file_descriptor);
		return CHAT_ERR_SYS;
	}

	client->socket = file_descriptor;
	client->feed_buffer.clear();
	client->out_buffer.clear();
	client->out_offset = 0;
	client->input.buffer.clear();
	client->input.offset = 0;
#if NEED_AUTHOR
	client->name_sent = false;

	// handshake: отправить имя автора один раз
	enqueueFrame(client->out_buffer, client->name, std::string_view());
	client->name_sent = true;
	if (!clientFlush(client)) {
		close(client->socket);
		client->socket = -1;
		return CHAT_ERR_SYS;
	}
#endif

	return 0;
}

chat_message *chat_client_pop_next(chat_client *client) {
	if (client == nullptr || client->incoming.empty())
		return nullptr;

	chat_message *message = client->incoming.front();
	client->incoming.pop_front();
	return message;
}

int chat_client_update(chat_client *client, const double timeout) {
	if (client == nullptr)
		return CHAT_ERR_INVALID_ARGUMENT;
	if (client->socket < 0)
		return CHAT_ERR_NOT_STARTED;

	const int events = chat_events_to_poll_events(chat_client_get_events(client));

	pollfd pfd {};
	std::memset(&pfd, 0, sizeof(pfd));
	pfd.fd = client->socket;
	pfd.events = static_cast<short>(events);

	int timeout_ms = -1;
	if (timeout >= 0) {
		double current_ms = timeout * 1000.0;
		if (current_ms > static_cast<double>(INT_MAX))
			current_ms = static_cast<double>(INT_MAX);
		if (current_ms < 0)
			current_ms = 0;
		timeout_ms = std::lround(static_cast<int>(current_ms) + 0.5);
	}

	int result = 0;
	while (true) {
		result = poll(&pfd, 1, timeout_ms);
		if (result < 0 && errno == EINTR)
			continue;
		break;
	}

	if (result < 0)
		return CHAT_ERR_SYS;
	if (result == 0)
		return CHAT_ERR_TIMEOUT;

	bool is_ok = true;

	if ((pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0)
		is_ok = false;
	if (is_ok && (pfd.revents & POLLIN) != 0)
		is_ok = clientRead(client);
	if (is_ok && (pfd.revents & POLLOUT) != 0)
		is_ok = clientFlush(client);
	if (is_ok && client->out_offset < client->out_buffer.size())
		is_ok = clientFlush(client);

	if (!is_ok) {
		close(client->socket);
		client->socket = -1;
		return CHAT_ERR_SYS;
	}

	return 0;
}

int chat_client_get_descriptor(const chat_client *client) {
	if (client == nullptr)
		return -1;
	return client->socket;
}

int chat_client_get_events(const chat_client *client) {
	if (client == nullptr || client->socket < 0)
		return 0;

	int mask = CHAT_EVENT_INPUT;
	if (client->out_offset < client->out_buffer.size())
		mask |= CHAT_EVENT_OUTPUT;
	return mask;
}

int chat_client_feed(chat_client *client, const char *message, const uint32_t msg_size) {
	if (client == nullptr || message == nullptr)
		return CHAT_ERR_INVALID_ARGUMENT;
	if (client->socket < 0)
		return CHAT_ERR_NOT_STARTED;

	client->feed_buffer.append(message, message + msg_size);

	while (true) {
		const size_t position = client->feed_buffer.find('\n');
		if (position == std::string::npos)
			break;

		const std::string_view line(client->feed_buffer.data(), position);
		std::string trimmed = trimCopy(line);
		client->feed_buffer.erase(0, position + 1);

		// пустые строки не отправляем
		if (trimmed.empty())
			continue;

#if NEED_AUTHOR
		// обычные сообщения идут с пустым author:
		// имя уже было отправлено handshake-ом
		enqueueFrame(client->out_buffer, std::string_view(), trimmed);
#else
		enqueueFrame(client->out_buffer, std::string_view(), trimmed);
#endif

		if (!clientFlush(client))
			return CHAT_ERR_SYS;
	}

	return 0;
}