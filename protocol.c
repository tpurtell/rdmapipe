#define _POSIX_C_SOURCE 200809L

#include "protocol.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define RP_ARGV_MAX (1024U * 1024U)

struct json_cursor {
	const char *p;
	const char *end;
};

static void skip_space(struct json_cursor *cursor)
{
	while (cursor->p < cursor->end
	 && (*cursor->p == ' ' || *cursor->p == '\t'
	     || *cursor->p == '\n' || *cursor->p == '\r'))
		cursor->p++;
}

static int consume(struct json_cursor *cursor, char wanted)
{
	skip_space(cursor);
	if (cursor->p == cursor->end || *cursor->p != wanted)
		return -1;
	cursor->p++;
	return 0;
}

static int parse_string(struct json_cursor *cursor, char *output, size_t size)
{
	size_t used = 0;

	skip_space(cursor);
	if (cursor->p == cursor->end || *cursor->p++ != '"')
		return -1;
	while (cursor->p < cursor->end && *cursor->p != '"') {
		unsigned char ch = (unsigned char)*cursor->p++;
		if (ch < 0x20 || ch == '\\' || used + 1 >= size)
			return -1;
		output[used++] = (char)ch;
	}
	if (cursor->p == cursor->end || *cursor->p++ != '"')
		return -1;
	output[used] = '\0';
	return 0;
}

static int skip_string(struct json_cursor *cursor)
{
	skip_space(cursor);
	if (cursor->p == cursor->end || *cursor->p++ != '"')
		return -1;
	while (cursor->p < cursor->end) {
		unsigned char ch = (unsigned char)*cursor->p++;
		unsigned i;

		if (ch == '"')
			return 0;
		if (ch < 0x20)
			return -1;
		if (ch != '\\')
			continue;
		if (cursor->p == cursor->end)
			return -1;
		ch = (unsigned char)*cursor->p++;
		if (strchr("\"\\/bfnrt", ch))
			continue;
		if (ch != 'u')
			return -1;
		for (i = 0; i < 4; i++) {
			unsigned char digit;
			if (cursor->p == cursor->end)
				return -1;
			digit = (unsigned char)*cursor->p++;
			if (!((digit >= '0' && digit <= '9')
			   || (digit >= 'a' && digit <= 'f')
			   || (digit >= 'A' && digit <= 'F')))
				return -1;
		}
	}
	return -1;
}

static int parse_u32(struct json_cursor *cursor, uint32_t *value)
{
	uint64_t result = 0;
	int digits = 0;

	skip_space(cursor);
	while (cursor->p < cursor->end
	 && *cursor->p >= '0' && *cursor->p <= '9') {
		result = result * 10 + (unsigned)(*cursor->p++ - '0');
		if (result > UINT32_MAX)
			return -1;
		digits++;
	}
	if (!digits)
		return -1;
	*value = (uint32_t)result;
	return 0;
}

static int skip_value(struct json_cursor *cursor, unsigned depth);

static int skip_compound(struct json_cursor *cursor, char open, char close,
			 unsigned depth)
{
	int first = 1;

	if (depth > 8 || consume(cursor, open) < 0)
		return -1;
	for (;;) {
		skip_space(cursor);
		if (cursor->p < cursor->end && *cursor->p == close) {
			cursor->p++;
			return 0;
		}
		if (!first && consume(cursor, ',') < 0)
			return -1;
		if (open == '{') {
			if (skip_string(cursor) < 0 || consume(cursor, ':') < 0)
				return -1;
		}
		if (skip_value(cursor, depth + 1) < 0)
			return -1;
		first = 0;
	}
}

static int skip_value(struct json_cursor *cursor, unsigned depth)
{
	uint32_t number;

	skip_space(cursor);
	if (cursor->p == cursor->end)
		return -1;
	if (*cursor->p == '"')
		return skip_string(cursor);
	if (*cursor->p == '{')
		return skip_compound(cursor, '{', '}', depth);
	if (*cursor->p == '[')
		return skip_compound(cursor, '[', ']', depth);
	if (*cursor->p >= '0' && *cursor->p <= '9')
		return parse_u32(cursor, &number);
	if ((size_t)(cursor->end - cursor->p) >= 4
	 && (!memcmp(cursor->p, "true", 4) || !memcmp(cursor->p, "null", 4))) {
		cursor->p += 4;
		return 0;
	}
	if ((size_t)(cursor->end - cursor->p) >= 5 && !memcmp(cursor->p, "false", 5)) {
		cursor->p += 5;
		return 0;
	}
	return -1;
}

static int parse_endpoint(struct json_cursor *cursor, struct rp_endpoint *endpoint)
{
	unsigned seen = 0;
	int first = 1;

	memset(endpoint, 0, sizeof *endpoint);
	if (consume(cursor, '{') < 0)
		return -1;
	for (;;) {
		char key[32];
		uint32_t number;

		skip_space(cursor);
		if (cursor->p < cursor->end && *cursor->p == '}') {
			cursor->p++;
			break;
		}
		if (!first && consume(cursor, ',') < 0)
			return -1;
		if (parse_string(cursor, key, sizeof key) < 0 || consume(cursor, ':') < 0)
			return -1;
		if (!strcmp(key, "address")) {
			if ((seen & 1) || parse_string(cursor, endpoint->address,
							 sizeof endpoint->address) < 0)
				return -1;
			seen |= 1;
		} else if (!strcmp(key, "port")) {
			if ((seen & 2) || parse_u32(cursor, &number) < 0 || !number || number > 65535)
				return -1;
			endpoint->port = (uint16_t)number;
			seen |= 2;
		} else if (!strcmp(key, "rate")) {
			if ((seen & 4) || parse_u32(cursor, &number) < 0 || number > 1000000)
				return -1;
			endpoint->rate_gbps = number;
			seen |= 4;
		} else if (skip_value(cursor, 0) < 0) {
			return -1;
		}
		first = 0;
	}
	if (seen != 7 || inet_pton(AF_INET, endpoint->address, &(struct in_addr){0}) != 1)
		return -1;
	return 0;
}

static int parse_endpoints(struct json_cursor *cursor, struct rp_descriptor *descriptor)
{
	int first = 1;

	if (consume(cursor, '[') < 0)
		return -1;
	for (;;) {
		skip_space(cursor);
		if (cursor->p < cursor->end && *cursor->p == ']') {
			cursor->p++;
			break;
		}
		if (!first && consume(cursor, ',') < 0)
			return -1;
		if (descriptor->endpoint_count == RP_MAX_ENDPOINTS
		 || parse_endpoint(cursor, &descriptor->endpoints[descriptor->endpoint_count]) < 0)
			return -1;
		descriptor->endpoint_count++;
		first = 0;
	}
	return descriptor->endpoint_count ? 0 : -1;
}

static int hex_nibble(char ch)
{
	if (ch >= '0' && ch <= '9')
		return ch - '0';
	if (ch >= 'a' && ch <= 'f')
		return ch - 'a' + 10;
	if (ch >= 'A' && ch <= 'F')
		return ch - 'A' + 10;
	return -1;
}

static int parse_token(struct json_cursor *cursor, unsigned char token[16])
{
	char text[33] = { 0 };
	unsigned i;

	if (parse_string(cursor, text, sizeof text) < 0 || strlen(text) != 32)
		return -1;
	for (i = 0; i < 16; i++) {
		int high = hex_nibble(text[i * 2]);
		int low = hex_nibble(text[i * 2 + 1]);
		if (high < 0 || low < 0)
			return -1;
		token[i] = (unsigned char)((high << 4) | low);
	}
	return 0;
}

int rp_descriptor_parse(const char *input, struct rp_descriptor *descriptor)
{
	struct json_cursor cursor;
	unsigned seen = 0;
	int first = 1;

	if (!input || strlen(input) >= RP_MAX_DESCRIPTOR) {
		rp_set_error("descriptor is empty or exceeds %u bytes", RP_MAX_DESCRIPTOR);
		return RP_EXIT_USAGE;
	}
	memset(descriptor, 0, sizeof *descriptor);
	cursor.p = input;
	cursor.end = input + strlen(input);
	if (consume(&cursor, '{') < 0)
		goto invalid;
	for (;;) {
		char key[32];

		skip_space(&cursor);
		if (cursor.p < cursor.end && *cursor.p == '}') {
			cursor.p++;
			break;
		}
		if (!first && consume(&cursor, ',') < 0)
			goto invalid;
		if (parse_string(&cursor, key, sizeof key) < 0 || consume(&cursor, ':') < 0)
			goto invalid;
		if (!strcmp(key, "rdmapipe")) {
			if ((seen & 1) || parse_u32(&cursor, &descriptor->version) < 0)
				goto invalid;
			seen |= 1;
		} else if (!strcmp(key, "token")) {
			if ((seen & 2) || parse_token(&cursor, descriptor->token) < 0)
				goto invalid;
			seen |= 2;
		} else if (!strcmp(key, "chunk")) {
			if ((seen & 4) || parse_u32(&cursor, &descriptor->chunk_size) < 0)
				goto invalid;
			seen |= 4;
		} else if (!strcmp(key, "depth")) {
			if ((seen & 8) || parse_u32(&cursor, &descriptor->queue_depth) < 0)
				goto invalid;
			seen |= 8;
		} else if (!strcmp(key, "channels")) {
			if ((seen & 16) || parse_u32(&cursor, &descriptor->channels) < 0)
				goto invalid;
			seen |= 16;
		} else if (!strcmp(key, "endpoints")) {
			if ((seen & 32) || parse_endpoints(&cursor, descriptor) < 0)
				goto invalid;
			seen |= 32;
		} else if (skip_value(&cursor, 0) < 0) {
			goto invalid;
		}
		first = 0;
	}
	skip_space(&cursor);
	if (cursor.p != cursor.end || seen != 63
	 || descriptor->version != RP_PROTOCOL_VERSION
	 || descriptor->chunk_size < 4096 || descriptor->chunk_size > 8U * 1024U * 1024U
	 || (descriptor->chunk_size & 63U)
	 || descriptor->queue_depth < 2 || descriptor->queue_depth > 4096
	 || descriptor->channels > RP_MAX_CHANNELS)
		goto invalid;
	return 0;

invalid:
	rp_set_error("invalid rdmapipe protocol-%u descriptor", RP_PROTOCOL_VERSION);
	return RP_EXIT_USAGE;
}

int rp_descriptor_format(const struct rp_descriptor *descriptor, char *output,
			 size_t output_size)
{
	static const char hex[] = "0123456789abcdef";
	char token[33];
	size_t used;
	unsigned i;
	int length;

	for (i = 0; i < 16; i++) {
		token[i * 2] = hex[descriptor->token[i] >> 4];
		token[i * 2 + 1] = hex[descriptor->token[i] & 15];
	}
	token[32] = '\0';
	length = snprintf(output, output_size,
		"{\"rdmapipe\":%u,\"token\":\"%s\",\"chunk\":%u,\"depth\":%u,\"channels\":%u,\"endpoints\":[",
		descriptor->version, token, descriptor->chunk_size,
		descriptor->queue_depth, descriptor->channels);
	if (length < 0 || (size_t)length >= output_size)
		goto overflow;
	used = (size_t)length;
	for (i = 0; i < descriptor->endpoint_count; i++) {
		const struct rp_endpoint *endpoint = &descriptor->endpoints[i];
		length = snprintf(output + used, output_size - used,
			"%s{\"address\":\"%s\",\"port\":%u,\"rate\":%u}",
			i ? "," : "", endpoint->address, endpoint->port,
			endpoint->rate_gbps);
		if (length < 0 || (size_t)length >= output_size - used)
			goto overflow;
		used += (size_t)length;
	}
	if (used + 4 > output_size)
		goto overflow;
	memcpy(output + used, "]}\n", 4);
	return (int)(used + 3);

overflow:
	rp_set_error("descriptor exceeds %u bytes", RP_MAX_DESCRIPTOR);
	return -1;
}

int rp_write_all(int fd, const void *data, size_t size)
{
	const unsigned char *position = data;

	while (size) {
		ssize_t result = write(fd, position, size);
		if (result < 0 && errno == EINTR)
			continue;
		if (result <= 0)
			return -1;
		position += result;
		size -= (size_t)result;
	}
	return 0;
}

static int descriptor_remaining_ms(const struct timespec *start, int timeout_ms)
{
	struct timespec now;
	int64_t elapsed;

	clock_gettime(CLOCK_MONOTONIC, &now);
	elapsed = (int64_t)(now.tv_sec - start->tv_sec) * 1000
		+ (now.tv_nsec - start->tv_nsec) / 1000000;
	if (elapsed >= timeout_ms)
		return 0;
	return timeout_ms - (int)elapsed;
}

int rp_descriptor_read(int fd, struct rp_descriptor *descriptor, int timeout_ms)
{
	char input[RP_MAX_DESCRIPTOR + 1];
	struct timespec start;
	size_t used = 0;

	if (timeout_ms <= 0) {
		rp_set_error("invalid descriptor timeout");
		return RP_EXIT_USAGE;
	}
	clock_gettime(CLOCK_MONOTONIC, &start);

	for (;;) {
		struct pollfd pollfd = { .fd = fd, .events = POLLIN };
		int ready, remaining;
		ssize_t result;
		char ch;

		if (used == RP_MAX_DESCRIPTOR) {
			rp_set_error("descriptor exceeds %u bytes", RP_MAX_DESCRIPTOR);
			return RP_EXIT_USAGE;
		}
		remaining = descriptor_remaining_ms(&start, timeout_ms);
		if (!remaining) {
			rp_set_error("timed out waiting for rendezvous descriptor");
			return RP_EXIT_TIMEOUT;
		}
		do {
			ready = poll(&pollfd, 1, remaining);
		} while (ready < 0 && errno == EINTR && !rp_interrupted);
		if (ready < 0) {
			if (rp_interrupted) {
				rp_set_error("interrupted while reading descriptor");
				return rp_interrupted_exit_code();
			}
			rp_set_error("cannot poll descriptor input: %s", strerror(errno));
			return RP_EXIT_IO;
		}
		if (!ready) {
			rp_set_error("timed out waiting for rendezvous descriptor");
			return RP_EXIT_TIMEOUT;
		}
		if (pollfd.revents & (POLLERR | POLLNVAL)) {
			rp_set_error("descriptor input failed");
			return RP_EXIT_IO;
		}
		result = read(fd, &ch, 1);
		if (result < 0 && errno == EINTR)
			continue;
		if (result < 0) {
			rp_set_error("cannot read descriptor: %s", strerror(errno));
			return RP_EXIT_IO;
		}
		if (!result) {
			rp_set_error("descriptor ended before newline");
			return RP_EXIT_USAGE;
		}
		if (ch == '\n')
			break;
		input[used++] = ch;
	}
	input[used] = '\0';
	return rp_descriptor_parse(input, descriptor);
}

int rp_random_token(unsigned char token[16])
{
	int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
	size_t used = 0;

	if (fd < 0) {
		rp_set_error("cannot open kernel random source: %s", strerror(errno));
		return RP_EXIT_IO;
	}
	while (used < 16) {
		ssize_t result = read(fd, token + used, 16 - used);
		if (result < 0 && errno == EINTR)
			continue;
		if (result <= 0) {
			rp_set_error("cannot read kernel random source: %s",
				result < 0 ? strerror(errno) : "unexpected EOF");
			close(fd);
			return RP_EXIT_IO;
		}
		used += (size_t)result;
	}
	close(fd);
	return 0;
}

static const char base64_alphabet[] =
	"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

char *rp_argv_encode(char *const argv[])
{
	size_t raw_size = 0, output_size, position = 0, i;
	unsigned char *raw;
	char *output;

	for (i = 0; argv[i]; i++) {
		size_t length = strlen(argv[i]) + 1;
		if (length > RP_ARGV_MAX - raw_size) {
			rp_set_error("remote argv exceeds %u bytes", RP_ARGV_MAX);
			return NULL;
		}
		raw_size += length;
	}
	if (!i) {
		rp_set_error("remote argv is empty");
		return NULL;
	}
	raw = malloc(raw_size);
	if (!raw) {
		rp_set_error("out of memory encoding remote argv");
		return NULL;
	}
	for (i = 0, position = 0; argv[i]; i++) {
		size_t length = strlen(argv[i]) + 1;
		memcpy(raw + position, argv[i], length);
		position += length;
	}
	output_size = ((raw_size + 2) / 3) * 4;
	output = malloc(output_size + 1);
	if (!output) {
		free(raw);
		rp_set_error("out of memory encoding remote argv");
		return NULL;
	}
	for (i = 0, position = 0; i < raw_size; i += 3) {
		uint32_t value = (uint32_t)raw[i] << 16;
		value |= i + 1 < raw_size ? (uint32_t)raw[i + 1] << 8 : 0;
		value |= i + 2 < raw_size ? raw[i + 2] : 0;
		output[position++] = base64_alphabet[(value >> 18) & 63];
		output[position++] = base64_alphabet[(value >> 12) & 63];
		output[position++] = i + 1 < raw_size ? base64_alphabet[(value >> 6) & 63] : '=';
		output[position++] = i + 2 < raw_size ? base64_alphabet[value & 63] : '=';
	}
	output[position] = '\0';
	free(raw);
	return output;
}

static int base64_value(unsigned char ch)
{
	const char *found = strchr(base64_alphabet, ch);
	return found ? (int)(found - base64_alphabet) : -1;
}

char **rp_argv_decode(const char *encoded)
{
	size_t length = strlen(encoded), raw_size, i, used = 0, argc = 0;
	unsigned char *raw;
	char **argv;

	if (!length || (length & 3) || length > ((RP_ARGV_MAX + 2) / 3) * 4) {
		rp_set_error("invalid encoded remote argv length");
		return NULL;
	}
	raw_size = length / 4 * 3;
	if (encoded[length - 1] == '=')
		raw_size--;
	if (encoded[length - 2] == '=')
		raw_size--;
	raw = malloc(raw_size + 1);
	if (!raw) {
		rp_set_error("out of memory decoding remote argv");
		return NULL;
	}
	for (i = 0; i < length; i += 4) {
		int a = base64_value((unsigned char)encoded[i]);
		int b = base64_value((unsigned char)encoded[i + 1]);
		int c = encoded[i + 2] == '=' ? 0 : base64_value((unsigned char)encoded[i + 2]);
		int d = encoded[i + 3] == '=' ? 0 : base64_value((unsigned char)encoded[i + 3]);
		uint32_t value;

		if (a < 0 || b < 0 || c < 0 || d < 0
		 || (i + 4 != length && (encoded[i + 2] == '=' || encoded[i + 3] == '='))
		 || (encoded[i + 2] == '=' && encoded[i + 3] != '=')
		 || (encoded[i + 2] == '=' && (b & 15))
		 || (encoded[i + 3] == '=' && encoded[i + 2] != '=' && (c & 3)))
			goto invalid;
		value = ((uint32_t)a << 18) | ((uint32_t)b << 12)
			| ((uint32_t)c << 6) | (uint32_t)d;
		if (used < raw_size)
			raw[used++] = (unsigned char)(value >> 16);
		if (used < raw_size)
			raw[used++] = (unsigned char)(value >> 8);
		if (used < raw_size)
			raw[used++] = (unsigned char)value;
	}
	if (used != raw_size || !raw_size || raw[raw_size - 1] != '\0')
		goto invalid;
	for (i = 0; i < raw_size; i++)
		if (!raw[i])
			argc++;
	argv = calloc(argc + 1, sizeof *argv);
	if (!argv) {
		free(raw);
		rp_set_error("out of memory decoding remote argv");
		return NULL;
	}
	for (i = 0, argc = 0; i < raw_size; i++) {
		argv[argc++] = (char *)raw + i;
		i += strlen((char *)raw + i);
	}
	return argv;

invalid:
	free(raw);
	rp_set_error("invalid encoded remote argv");
	return NULL;
}

void rp_argv_free(char **argv)
{
	if (argv) {
		free(argv[0]);
		free(argv);
	}
}
