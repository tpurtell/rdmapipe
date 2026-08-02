#ifndef RDMAPIPE_TRANSPORT_H
#define RDMAPIPE_TRANSPORT_H

#include "rdmapipe.h"

#include <sys/types.h>

int rp_send_stream(struct rp_config *config, int input_fd, int descriptor_fd);
int rp_receive_stream(struct rp_config *config, int descriptor_fd, int output_fd,
			 pid_t consumer_pid);

#endif
