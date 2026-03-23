#pragma once

#include <cstdint>

struct chat_server;

struct chat_server *chat_server_new(void);

void chat_server_delete(struct chat_server *server);

int chat_server_listen(struct chat_server *server, uint16_t port);

struct chat_message *chat_server_pop_next(struct chat_server *server);

int chat_server_update(struct chat_server *server, double timeout);

int chat_server_get_descriptor(const struct chat_server *server);

int chat_server_get_socket(const struct chat_server *server);

int chat_server_get_events(const struct chat_server *server);

int chat_server_feed(struct chat_server *server, const char *msg, uint32_t msg_size);
