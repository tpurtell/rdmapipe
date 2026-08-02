#ifndef RDMAPIPE_H
#define RDMAPIPE_H

#include <signal.h>
#include <stddef.h>
#include <stdint.h>

#define RP_VERSION "0.1.0"
#define RP_PROTOCOL_VERSION 1

#define RP_DEFAULT_CHUNK (2U * 1024U * 1024U)
#define RP_DEFAULT_DEPTH 4U
#define RP_DEFAULT_TIMEOUT 300U
#define RP_MAX_CHANNELS 2U
#define RP_MAX_ENDPOINTS 8U
#define RP_MAX_DESCRIPTOR 4096U
#define RP_MAX_DEVICE_FILTER 255U

enum rp_exit_code {
	RP_EXIT_OK = 0,
	RP_EXIT_USAGE = 2,
	RP_EXIT_UNAVAILABLE = 69,
	RP_EXIT_PROTOCOL = 70,
	RP_EXIT_IO = 74,
	RP_EXIT_TIMEOUT = 75
};

struct rp_config {
	uint32_t chunk_size;
	uint32_t queue_depth;
	uint32_t channels;
	uint32_t port;
	uint32_t timeout;
	int chunk_explicit;
	int depth_explicit;
	int channels_explicit;
	int discard;
	int quiet;
	int verbose;
	char device_filter[RP_MAX_DEVICE_FILTER + 1];
};

extern volatile sig_atomic_t rp_interrupted;

void rp_config_init(struct rp_config *config);
void rp_error(const char *format, ...);
void rp_log(const struct rp_config *config, int level, const char *format, ...);
const char *rp_last_error(void);
void rp_set_error(const char *format, ...);
uint64_t rp_hton64(uint64_t value);
uint64_t rp_ntoh64(uint64_t value);
int rp_interrupted_exit_code(void);

#endif
