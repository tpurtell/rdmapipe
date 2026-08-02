#ifndef RDMAPIPE_PROTOCOL_H
#define RDMAPIPE_PROTOCOL_H

#include "rdmapipe.h"

#include <netinet/in.h>

struct rp_endpoint {
	char address[INET_ADDRSTRLEN];
	uint16_t port;
	uint32_t rate_gbps;
};

struct rp_descriptor {
	uint32_t version;
	unsigned char token[16];
	uint32_t chunk_size;
	uint32_t queue_depth;
	uint32_t channels;
	size_t endpoint_count;
	struct rp_endpoint endpoints[RP_MAX_ENDPOINTS];
};

int rp_random_token(unsigned char token[16]);
int rp_descriptor_format(const struct rp_descriptor *descriptor, char *output,
			 size_t output_size);
int rp_descriptor_parse(const char *input, struct rp_descriptor *descriptor);
int rp_descriptor_read(int fd, struct rp_descriptor *descriptor, int timeout_ms);
int rp_write_all(int fd, const void *data, size_t size);

char *rp_argv_encode(char *const argv[]);
char **rp_argv_decode(const char *encoded);
void rp_argv_free(char **argv);

#endif
