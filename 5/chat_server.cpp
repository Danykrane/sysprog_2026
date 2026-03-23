#include "chat_server.h"

#include <arpa/inet.h>
#include <cerrno>
#include <climits>
#include <cstring>
#include <deque>
#include <memory>
#include <netinet/in.h>
#include <string>
#include <string_view>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

#include "chat.h"

struct chat_peer {
	int socket = -1;

	// буфер отправки клиенту
	std::string out_buffer;
	size_t out_offset = 0;

	// разбор входящих фреймов
	frame_parser input;

#if NEED_AUTHOR
	std::string author;
	bool has_author = false;
#endif
};

struct chat_server {
	// listen socket
	int socket = -1;
	// epoll descriptor
	int epoll_file_descriptor = -1;

	// подключенные клиенты
	std::vector<chat_peer *> peers;
	// очередь принятых сервером сообщений
	std::deque<chat_message *> incoming;

#if NEED_SERVER_FEED
	// незавершенная строка для feed администратора
	std::string admin_feed_buffer;
#endif
};

// удалить peer
void peer_destroy(const chat_server *server, const chat_peer *peer) {
	if (peer == nullptr)
		return;

	if (server->epoll_file_descriptor >= 0 && peer->socket >= 0)
		epoll_ctl(server->epoll_file_descriptor, EPOLL_CTL_DEL, peer->socket, nullptr);

	if (peer->socket >= 0)
		close(peer->socket);

	delete peer;
}

// убрать peer из массива и удалить
void server_remove_peer(chat_server *server, const chat_peer *peer) {
	for (size_t index = 0; index < server->peers.size(); ++index) {
		if (server->peers[index] == peer) {
			server->peers[index] = server->peers.back();
			server->peers.pop_back();
			break;
		}
	}

	peer_destroy(server, peer);
}

// дозаписать буфер peer-а в сокет
bool peer_flush(chat_peer *peer) {
	while (peer->socket >= 0 && peer->out_offset < peer->out_buffer.size()) {
		const size_t left = peer->out_buffer.size() - peer->out_offset;
		const ssize_t value = send(
			peer->socket,
			peer->out_buffer.data() + peer->out_offset,
			left,
			MSG_NOSIGNAL);

		if (value > 0) {
			peer->out_offset += static_cast<size_t>(value);
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

	if (peer->out_offset >= peer->out_buffer.size()) {
		peer->out_buffer.clear();
		peer->out_offset = 0;
	}

	return true;
}

// положить сообщение peer-у в очередь и попытаться сразу отправить
void peer_enqueue_and_try_flush(
	chat_peer *peer,
	const std::string_view author,
	const std::string_view data)
{
	enqueueFrame(peer->out_buffer, author, data);
	(void)peer_flush(peer);
}

// принять всех ожидающих клиентов
bool server_accept_pending(chat_server *server) {
	while (true) {
		sockaddr_in address {};
		socklen_t address_length = sizeof(address);

		const int file_descriptor =
			accept(server->socket, reinterpret_cast<sockaddr *>(&address), &address_length);

		if (file_descriptor < 0) {
			if (errno == EINTR)
				continue;
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				break;
			return false;
		}

		if (setNonBlocking(file_descriptor) != 0) {
			close(file_descriptor);
			return false;
		}

		auto *peer = new chat_peer();
		peer->socket = file_descriptor;
		server->peers.push_back(peer);

		// кладем peer в epoll через data.ptr
		epoll_event event {};
		std::memset(&event, 0, sizeof(event));
		event.events = EPOLLIN | EPOLLOUT | EPOLLET | EPOLLRDHUP;
		event.data.ptr = peer;

		if (epoll_ctl(server->epoll_file_descriptor, EPOLL_CTL_ADD, file_descriptor, &event) != 0) {
			server_remove_peer(server, peer);
			return false;
		}
	}

	return true;
}

// разослать сообщение всем, кроме sender
void server_broadcast(
	const chat_server *server,
	const chat_peer *sender,
	const std::string_view author,
	const std::string_view data)
{
	for (chat_peer *peer : server->peers) {
		if (sender != nullptr && peer == sender)
			continue;
		peer_enqueue_and_try_flush(peer, author, data);
	}
}

// считать все доступные данные от peer-а
bool server_peer_read(chat_server *server, chat_peer *peer) {
	char temporary[64 * 1024];

	while (true) {
		const ssize_t value = recv(peer->socket, temporary, sizeof(temporary), 0);

		if (value > 0) {
			peer->input.buffer.append(temporary, static_cast<size_t>(value));

			std::string parsed_author;
			std::string parsed_data;
			while (peer->input.try_pop(parsed_author, parsed_data)) {
#if NEED_AUTHOR
				// handshake: author есть, data пустая
				if (!peer->has_author && !parsed_author.empty() && parsed_data.empty()) {
					peer->author = std::move(parsed_author);
					peer->has_author = true;
					parsed_author.clear();
					parsed_data.clear();
					continue;
				}
#endif

				if (parsed_data.empty()) {
					parsed_author.clear();
					parsed_data.clear();
					continue;
				}

				auto message = std::make_unique<chat_message>();
#if NEED_AUTHOR
				message->author = peer->has_author ? peer->author : std::string();
#endif
				message->data = std::move(parsed_data);

				server->incoming.push_back(message.get());

#if NEED_AUTHOR
				const std::string_view author =
					peer->has_author ? std::string_view(peer->author) : std::string_view();
				server_broadcast(server, peer, author, message->data);
#else
				server_broadcast(server, peer, std::string_view(), message->data);
#endif

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

chat_server *chat_server_new() {
	return new chat_server();
}

void chat_server_delete(chat_server *server) {
	if (server == nullptr)
		return;

	while (!server->incoming.empty()) {
		delete server->incoming.front();
		server->incoming.pop_front();
	}

	for (const chat_peer *peer : server->peers)
		peer_destroy(server, peer);
	server->peers.clear();

	if (server->epoll_file_descriptor >= 0 && server->socket >= 0)
		epoll_ctl(server->epoll_file_descriptor, EPOLL_CTL_DEL, server->socket, nullptr);

	if (server->socket >= 0)
		close(server->socket);
	if (server->epoll_file_descriptor >= 0)
		close(server->epoll_file_descriptor);

	delete server;
}

int chat_server_listen(chat_server *server, const uint16_t port) {
	if (server == nullptr)
		return CHAT_ERR_INVALID_ARGUMENT;
	if (server->socket >= 0)
		return CHAT_ERR_ALREADY_STARTED;

	const int file_descriptor = socket(AF_INET, SOCK_STREAM, 0);
	if (file_descriptor < 0)
		return CHAT_ERR_SYS;

	constexpr int one = 1;
	setsockopt(file_descriptor, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

	sockaddr_in address {};
	std::memset(&address, 0, sizeof(address));
	address.sin_family = AF_INET;
	address.sin_port = htons(port);
	address.sin_addr.s_addr = htonl(INADDR_ANY);

	if (bind(file_descriptor, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0) {
		const int err = errno;
		close(file_descriptor);
		if (err == EADDRINUSE)
			return CHAT_ERR_PORT_BUSY;
		return CHAT_ERR_SYS;
	}

	if (listen(file_descriptor, 128) != 0) {
		close(file_descriptor);
		return CHAT_ERR_SYS;
	}

	if (setNonBlocking(file_descriptor) != 0) {
		close(file_descriptor);
		return CHAT_ERR_SYS;
	}

	const int efd = epoll_create1(0);
	if (efd < 0) {
		close(file_descriptor);
		return CHAT_ERR_SYS;
	}

	epoll_event event {};
	std::memset(&event, 0, sizeof(event));
	event.events = EPOLLIN | EPOLLET;
	event.data.ptr = server;

	if (epoll_ctl(efd, EPOLL_CTL_ADD, file_descriptor, &event) != 0) {
		close(efd);
		close(file_descriptor);
		return CHAT_ERR_SYS;
	}

	server->socket = file_descriptor;
	server->epoll_file_descriptor = efd;
#if NEED_SERVER_FEED
	server->admin_feed_buffer.clear();
#endif
	return 0;
}

chat_message *chat_server_pop_next(chat_server *server) {
	if (server == nullptr || server->incoming.empty())
		return nullptr;

	chat_message *message = server->incoming.front();
	server->incoming.pop_front();
	return message;
}

int chat_server_update(chat_server *server, const double timeout) {
	if (server == nullptr)
		return CHAT_ERR_INVALID_ARGUMENT;
	if (server->socket < 0 || server->epoll_file_descriptor < 0)
		return CHAT_ERR_NOT_STARTED;

	int timeout_ms = -1;
	if (timeout >= 0) {
		double current_ms = timeout * 1000.0;
		if (current_ms > static_cast<double>(INT_MAX))
			current_ms = static_cast<double>(INT_MAX);
		if (current_ms < 0)
			current_ms = 0;
		timeout_ms = static_cast<int>(current_ms + 0.5);
	}

	epoll_event events[64];
	int value = 0;

	while (true) {
		value = epoll_wait(server->epoll_file_descriptor, events, 64, timeout_ms);
		if (value < 0 && errno == EINTR)
			continue;
		break;
	}

	if (value < 0)
		return CHAT_ERR_SYS;
	if (value == 0)
		return CHAT_ERR_TIMEOUT;

	for (int index = 0; index < value; ++index) {
		void *tag = events[index].data.ptr;
		const uint32_t event = events[index].events;

		// событие на listen socket
		if (tag == server) {
			if (!server_accept_pending(server))
				return CHAT_ERR_SYS;
			continue;
		}

		auto *peer = static_cast<chat_peer *>(tag);
		bool alive = true;

		if ((event & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) != 0) {
			(void)server_peer_read(server, peer);
			alive = false;
		}

		if (alive && (event & EPOLLIN) != 0)
			alive = server_peer_read(server, peer);
		if (alive && (event & EPOLLOUT) != 0)
			alive = peer_flush(peer);

		// в edge-triggered режиме стараемся выкачать все сразу
		if (alive && peer->out_offset < peer->out_buffer.size())
			alive = peer_flush(peer);

		if (!alive)
			server_remove_peer(server, peer);
	}

	return 0;
}

int chat_server_get_descriptor(const chat_server *server) {
	if (server == nullptr)
		return -1;
	return server->epoll_file_descriptor;
}

int chat_server_get_socket(const chat_server *server) {
	if (server == nullptr)
		return -1;
	return server->socket;
}

int chat_server_get_events(const chat_server *server) {
	if (server == nullptr || server->socket < 0)
		return 0;

	int mask = CHAT_EVENT_INPUT;

	for (const chat_peer *peer : server->peers) {
		if (peer->out_offset < peer->out_buffer.size()) {
			mask |= CHAT_EVENT_OUTPUT;
			break;
		}
	}

	return mask;
}

int chat_server_feed(chat_server *server, const char *message, const uint32_t msg_size) {
#if !NEED_SERVER_FEED
	(void)server;
	(void)message;
	(void)msg_size;
	return CHAT_ERR_NOT_IMPLEMENTED;
#else
	if (server == nullptr || message == nullptr)
		return CHAT_ERR_INVALID_ARGUMENT;
	if (server->socket < 0)
		return CHAT_ERR_NOT_STARTED;

	// даже если update еще не вызывали, клиентов надо принять
	if (!server_accept_pending(server))
		return CHAT_ERR_SYS;

	server->admin_feed_buffer.append(message, message + msg_size);

	while (true) {
		const size_t position = server->admin_feed_buffer.find('\n');
		if (position == std::string::npos)
			break;

		const std::string_view line(server->admin_feed_buffer.data(), position);
		std::string trimmed = trimCopy(line);
		server->admin_feed_buffer.erase(0, position + 1);

		if (trimmed.empty())
			continue;

#if NEED_AUTHOR
		server_broadcast(server, nullptr, std::string_view("server"), trimmed);
#else
		server_broadcast(server, nullptr, std::string_view(), trimmed);
#endif
	}

	return 0;
#endif
}