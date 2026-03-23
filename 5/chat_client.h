#pragma once

#include <cstdint>
#include <string_view>

struct chat_client;

struct chat_client *chat_client_new(std::string_view name);

void chat_client_delete(struct chat_client *client);

int chat_client_connect(struct chat_client *client, std::string_view addr);

struct chat_message *chat_client_pop_next(struct chat_client *client);

int chat_client_update(struct chat_client *client, double timeout);

int chat_client_get_descriptor(const struct chat_client *client);

int chat_client_get_events(const struct chat_client *client);

int chat_client_feed(struct chat_client *client, const char *msg, uint32_t msg_size);
