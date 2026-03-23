#include "chat.h"
#include <arpa/inet.h>
#include <fcntl.h>
#include <poll.h>

#include <cctype>
#include <cstring>

int setNonBlocking(const int file_descriptor) {
	const int flags = fcntl(file_descriptor, F_GETFL, 0);
	if (flags < 0)
		return -1;
	if (fcntl(file_descriptor, F_SETFL, flags | O_NONBLOCK) < 0)
		return -1;
	return 0;
}

bool isSpace(const char character) {
	return std::isspace(static_cast<unsigned char>(character)) != 0;
}

std::string trimCopy(const std::string_view string) {
	size_t begin = 0;
	while (begin < string.size() && isSpace(string[begin]))
		++begin;

	size_t end = string.size();
	while (end > begin && isSpace(string[end - 1]))
		--end;

	return std::string(string.substr(begin, end - begin));
}

void appendU32(std::string &buffer, const uint32_t value) {
	const uint32_t network_value = htonl(value);
	buffer.append(reinterpret_cast<const char *>(&network_value), sizeof(network_value));
}

void readU32(const char *pointer_to_data, uint32_t &out) {
	uint32_t network_value = 0;
	std::memcpy(&network_value, pointer_to_data, sizeof(network_value));
	out = ntohl(network_value);
}

void enqueueFrame(std::string &out, const std::string_view author, const std::string_view data) {
	appendU32(out, static_cast<uint32_t>(author.size()));
	appendU32(out, static_cast<uint32_t>(data.size()));

	if (!author.empty())
		out.append(author.data(), author.size());
	if (!data.empty())
		out.append(data.data(), data.size());
}

bool frame_parser::try_pop(std::string &author, std::string &data) {
	author.clear();
	data.clear();

	const size_t available = buffer.size() - offset;
	if (available < 8)
		return false;

	uint32_t author_length = 0;
	uint32_t data_length = 0;
	readU32(buffer.data() + offset, author_length);
	readU32(buffer.data() + offset + 4, data_length);

	const uint64_t need =
		8ull + static_cast<uint64_t>(author_length) + static_cast<uint64_t>(data_length);

	if (static_cast<uint64_t>(available) < need)
		return false;

	const char *pointer = buffer.data() + offset + 8;

	if (author_length != 0)
		author.assign(pointer, author_length);
	pointer += author_length;

	if (data_length != 0)
		data.assign(pointer, data_length);

	offset += static_cast<size_t>(need);

	// периодически сжимать буфер
	if (offset == buffer.size()) {
		buffer.clear();
		offset = 0;
	} else if (offset > 65536) {
		buffer.erase(0, offset);
		offset = 0;
	}

	return true;
}

int parseAddress(const std::string_view address, std::string &host, std::string &port) {
	// формат host:port, без IPv6
	const size_t position = address.rfind(':');
	if (position == std::string_view::npos || position == 0 || position + 1 >= address.size())
		return CHAT_ERR_NO_ADDR;

	host.assign(address.substr(0, position));
	port.assign(address.substr(position + 1));
	return 0;
}

int chat_events_to_poll_events(const int mask) {
	int result = 0;

	if ((mask & CHAT_EVENT_INPUT) != 0)
		result |= POLLIN;
	if ((mask & CHAT_EVENT_OUTPUT) != 0)
		result |= POLLOUT;

	return result;
}