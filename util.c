#define _POSIX_C_SOURCE 200809L

#include "rdmapipe.h"

#include <arpa/inet.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

volatile sig_atomic_t rp_interrupted;

static char error_buffer[512];

void rp_config_init(struct rp_config *config)
{
	memset(config, 0, sizeof *config);
	config->chunk_size = RP_DEFAULT_CHUNK;
	config->queue_depth = RP_DEFAULT_DEPTH;
	config->timeout = RP_DEFAULT_TIMEOUT;
}

void rp_set_error(const char *format, ...)
{
	va_list arguments;
	va_start(arguments, format);
	vsnprintf(error_buffer, sizeof error_buffer, format, arguments);
	va_end(arguments);
}

const char *rp_last_error(void)
{
	return error_buffer[0] ? error_buffer : "unknown error";
}

void rp_error(const char *format, ...)
{
	va_list arguments;
	fputs("rdmapipe: ", stderr);
	va_start(arguments, format);
	vfprintf(stderr, format, arguments);
	va_end(arguments);
	fputc('\n', stderr);
}

void rp_log(const struct rp_config *config, int level, const char *format, ...)
{
	va_list arguments;

	if (config->quiet || config->verbose < level)
		return;
	fputs("rdmapipe: ", stderr);
	va_start(arguments, format);
	vfprintf(stderr, format, arguments);
	va_end(arguments);
	fputc('\n', stderr);
}

uint64_t rp_hton64(uint64_t value)
{
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
	return ((uint64_t)htonl((uint32_t)value) << 32) | htonl((uint32_t)(value >> 32));
#else
	return value;
#endif
}

uint64_t rp_ntoh64(uint64_t value)
{
	return rp_hton64(value);
}

int rp_interrupted_exit_code(void)
{
	int signal_number = rp_interrupted > 0 && rp_interrupted < 128
		? (int)rp_interrupted : SIGINT;
	return 128 + signal_number;
}
