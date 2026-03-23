#pragma once

#define NEED_AUTHOR 1
#define NEED_SERVER_FEED 1

#include <cstdint>
#include <string>
#include <string_view>

enum chat_errcode {
	CHAT_ERR_INVALID_ARGUMENT = 1,
	CHAT_ERR_TIMEOUT,
	CHAT_ERR_PORT_BUSY,
	CHAT_ERR_NO_ADDR,
	CHAT_ERR_ALREADY_STARTED,
	CHAT_ERR_NOT_IMPLEMENTED,
	CHAT_ERR_NOT_STARTED,
	CHAT_ERR_SYS,
};

enum chat_events {
	CHAT_EVENT_INPUT = 1,
	CHAT_EVENT_OUTPUT = 2,
};

struct chat_message {
#if NEED_AUTHOR
	std::string author;
#endif
	std::string data;
};

int setNonBlocking(int file_descriptor);

bool isSpace(char character);

std::string trimCopy(std::string_view string);

void appendU32(std::string &buffer, uint32_t value);

void readU32(const char *pointer_to_data, uint32_t &out);

void enqueueFrame(std::string &out, std::string_view author, std::string_view data);

struct frame_parser {
	std::string buffer;
	size_t offset = 0;

	// попробовать вытащить один полный фрейм
	bool try_pop(std::string &author, std::string &data);
};

// разобрать host:port
int parseAddress(std::string_view address, std::string &host, std::string &port);

// перевести маску событий чата в poll events
int chat_events_to_poll_events(int mask);