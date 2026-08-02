#define _GNU_SOURCE

#include "transport.h"
#include "protocol.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <infiniband/verbs.h>
#include <limits.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define RP_CONTROL_MAGIC 0x52445043U /* RDPC */
#define RP_FRAME_MAGIC 0x52445044U   /* RDPD */
#define RP_BOOTSTRAP_SIZE 72U
#define RP_STATUS_SIZE 96U
#define RP_NAME_SIZE 64U

#define RP_FRAME_DATA 1U
#define RP_FRAME_FIN 2U
#define RP_FRAME_PROBE 3U

#define RP_STATUS_OK 0U
#define RP_STATUS_ERROR 1U
#define RP_STATUS_CANCEL 2U

struct rp_candidate {
	char ibdev[RP_NAME_SIZE];
	char netdev[IFNAMSIZ];
	char address[INET_ADDRSTRLEN];
	struct sockaddr_in sin;
	union ibv_gid gid;
	uint32_t rate_gbps;
	int port_num;
	int gid_index;
	int listen_fd;
	uint16_t listen_port;
};

struct rp_frame_header {
	uint32_t magic;
	uint32_t type;
	uint32_t length;
	uint32_t reserved;
	uint64_t sequence;
};

_Static_assert(sizeof(struct rp_frame_header) == 24, "frame header must be 24 bytes");

struct rp_path {
	struct rp_candidate candidate;
	struct ibv_context *context;
	struct ibv_pd *pd;
	struct ibv_cq *cq;
	struct ibv_qp *qp;
	struct ibv_mr *mr;
	unsigned char *ring;
	size_t stride;
	uint32_t depth;
	uint32_t outstanding;
	uint32_t next_slot;
};

struct rp_session {
	struct rp_config *config;
	int sender;
	int control_fd;
	uint32_t channel_count;
	size_t candidate_count;
	struct rp_candidate candidates[RP_MAX_ENDPOINTS];
	struct rp_path paths[RP_MAX_CHANNELS];
	unsigned char token[16];
	uint64_t bytes;
	uint64_t messages;
	struct timespec started;
	struct timespec finished;
	int timer_started;
	int input_nonblocking;
};

struct rp_bootstrap {
	uint32_t channel;
	uint32_t channels;
	uint32_t endpoint;
	uint32_t chunk_size;
	uint32_t queue_depth;
	uint32_t qpn;
	uint32_t psn;
	uint32_t mtu;
	union ibv_gid gid;
	unsigned char token[16];
};

struct rp_status {
	uint32_t state;
	uint32_t code;
	uint64_t bytes;
	uint64_t messages;
	unsigned char token[16];
	char reason[49];
};

static int elapsed_ms(const struct timespec *start)
{
	struct timespec now;
	int64_t value;

	clock_gettime(CLOCK_MONOTONIC, &now);
	value = (int64_t)(now.tv_sec - start->tv_sec) * 1000
		+ (now.tv_nsec - start->tv_nsec) / 1000000;
	return value > INT_MAX ? INT_MAX : (int)value;
}

static void timer_start(struct rp_session *session)
{
	if (!session->timer_started) {
		clock_gettime(CLOCK_MONOTONIC, &session->started);
		session->timer_started = 1;
	}
}

static int timeout_ms(const struct rp_session *session)
{
	return (int)(session->config->timeout * 1000U);
}

static int wait_fd(int fd, short events, int milliseconds)
{
	struct pollfd pollfd = { .fd = fd, .events = events };
	int result;

	do {
		result = poll(&pollfd, 1, milliseconds);
	} while (result < 0 && errno == EINTR && !rp_interrupted);
	if (result <= 0)
		return result;
	if (pollfd.revents & (POLLERR | POLLNVAL))
		return -1;
	return pollfd.revents & (events | POLLHUP) ? 1 : 0;
}

static int socket_write_all(int fd, const void *data, size_t size, int milliseconds)
{
	const unsigned char *position = data;
	struct timespec start;

	clock_gettime(CLOCK_MONOTONIC, &start);
	while (size && !rp_interrupted) {
		int remaining = milliseconds - elapsed_ms(&start);
		ssize_t result;
		int ready;
		if (remaining <= 0) {
			errno = ETIMEDOUT;
			return -1;
		}
		ready = wait_fd(fd, POLLOUT, remaining);
		if (!ready) {
			errno = ETIMEDOUT;
			return -1;
		}
		if (ready < 0)
			return -1;
		result = send(fd, position, size, MSG_NOSIGNAL);
		if (result < 0 && errno == EINTR)
			continue;
		if (!result) {
			errno = EPIPE;
			return -1;
		}
		if (result < 0)
			return -1;
		position += result;
		size -= (size_t)result;
	}
	return size ? -1 : 0;
}

static int socket_read_all(int fd, void *data, size_t size, int milliseconds)
{
	unsigned char *position = data;
	struct timespec start;

	clock_gettime(CLOCK_MONOTONIC, &start);
	while (size && !rp_interrupted) {
		int remaining = milliseconds - elapsed_ms(&start);
		ssize_t result;
		int ready;
		if (remaining <= 0) {
			errno = ETIMEDOUT;
			return -1;
		}
		ready = wait_fd(fd, POLLIN, remaining);
		if (!ready) {
			errno = ETIMEDOUT;
			return -1;
		}
		if (ready < 0)
			return -1;
		result = read(fd, position, size);
		if (result < 0 && errno == EINTR)
			continue;
		if (!result) {
			errno = ECONNRESET;
			return -1;
		}
		if (result < 0)
			return -1;
		position += result;
		size -= (size_t)result;
	}
	return size ? -1 : 0;
}

static int control_io_exit_code(int error_number)
{
	if (rp_interrupted)
		return rp_interrupted_exit_code();
	return error_number == ETIMEDOUT ? RP_EXIT_TIMEOUT : RP_EXIT_IO;
}

static int token_equal(const unsigned char left[16], const unsigned char right[16])
{
	unsigned char difference = 0;
	size_t i;

	for (i = 0; i < 16; i++)
		difference |= left[i] ^ right[i];
	return difference == 0;
}

static int read_text_file(const char *path, char *output, size_t output_size)
{
	int fd;
	ssize_t length;

	if (output_size < 2 || (fd = open(path, O_RDONLY | O_CLOEXEC)) < 0)
		return -1;
	do {
		length = read(fd, output, output_size - 1);
	} while (length < 0 && errno == EINTR);
	close(fd);
	if (length <= 0)
		return -1;
	output[length] = '\0';
	while (length > 0 && isspace((unsigned char)output[length - 1]))
		output[--length] = '\0';
	return 0;
}

static int prepare_input(struct rp_session *session, int input_fd)
{
	int flags = fcntl(input_fd, F_GETFL, 0);

	if (flags < 0)
		return -1;
#ifdef F_GETPIPE_SZ
	{
		int current = fcntl(input_fd, F_GETPIPE_SZ);
		if (current >= 0) {
			char limit_text[64];
			uint32_t target = session->config->chunk_size;
			int actual = current;

			if (!read_text_file("/proc/sys/fs/pipe-max-size", limit_text,
					    sizeof limit_text)) {
				unsigned long limit = strtoul(limit_text, NULL, 10);
				if (limit && limit < target)
					target = (uint32_t)limit;
			}
			if (target > (uint32_t)current) {
				int resized = fcntl(input_fd, F_SETPIPE_SZ, (int)target);
				if (resized >= 0)
					actual = resized;
			}
			rp_log(session->config, 2, "input pipe capacity: %d bytes", actual);
		}
	}
#endif
	if (!(flags & O_NONBLOCK)) {
		if (fcntl(input_fd, F_SETFL, flags | O_NONBLOCK) < 0)
			return -1;
	}
	session->input_nonblocking = 1;
	return flags;
}

static int restore_input(struct rp_session *session, int input_fd, int flags)
{
	if (flags >= 0 && !(flags & O_NONBLOCK)
	 && fcntl(input_fd, F_SETFL, flags) < 0) {
		rp_set_error("cannot restore stdin flags: %s", strerror(errno));
		session->input_nonblocking = 0;
		return RP_EXIT_IO;
	}
	session->input_nonblocking = 0;
	return 0;
}

static int find_ipv4(const char *netdev, struct sockaddr_in *sin, char *text,
		     size_t text_size)
{
	struct ifaddrs *addresses, *address;
	int found = 0;

	if (getifaddrs(&addresses) < 0)
		return 0;
	for (address = addresses; address; address = address->ifa_next) {
		if (!address->ifa_addr || address->ifa_addr->sa_family != AF_INET
		 || strcmp(address->ifa_name, netdev) || !(address->ifa_flags & IFF_UP))
			continue;
		memcpy(sin, address->ifa_addr, sizeof *sin);
		if (inet_ntop(AF_INET, &sin->sin_addr, text, (socklen_t)text_size)) {
			found = 1;
			break;
		}
	}
	freeifaddrs(addresses);
	return found;
}

static int gid_matches_ipv4(const union ibv_gid *gid, const struct in_addr *address)
{
	static const unsigned char prefix[12] = {
		0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff
	};
	return !memcmp(gid->raw, prefix, sizeof prefix)
		&& !memcmp(gid->raw + 12, address, sizeof *address);
}

static int find_gid(const char *ibdev, const char *netdev,
		    const struct in_addr *address, int *gid_index, union ibv_gid *gid)
{
	char directory_path[PATH_MAX], path[PATH_MAX], value[128], type[64];
	DIR *directory;
	struct dirent *entry;
	int found = 0;

	snprintf(directory_path, sizeof directory_path,
		 "/sys/class/infiniband/%s/ports/1/gid_attrs/ndevs", ibdev);
	if (!(directory = opendir(directory_path)))
		return 0;
	while ((entry = readdir(directory))) {
		char *end;
		long index;

		if (!isdigit((unsigned char)entry->d_name[0]))
			continue;
		index = strtol(entry->d_name, &end, 10);
		if (*end || index < 0 || index > INT_MAX)
			continue;
		if (snprintf(path, sizeof path, "%s/%s", directory_path, entry->d_name)
		    >= (int)sizeof path)
			continue;
		if (read_text_file(path, value, sizeof value) < 0 || strcmp(value, netdev))
			continue;
		snprintf(path, sizeof path,
			 "/sys/class/infiniband/%s/ports/1/gid_attrs/types/%s",
			 ibdev, entry->d_name);
		if (read_text_file(path, type, sizeof type) < 0 || strcmp(type, "RoCE v2"))
			continue;
		snprintf(path, sizeof path, "/sys/class/infiniband/%s/ports/1/gids/%s",
			 ibdev, entry->d_name);
		if (read_text_file(path, value, sizeof value) < 0
		 || inet_pton(AF_INET6, value, gid->raw) != 1
		 || !gid_matches_ipv4(gid, address))
			continue;
		*gid_index = (int)index;
		found = 1;
		break;
	}
	closedir(directory);
	return found;
}

static int filter_matches(const char *filter, const struct rp_candidate *candidate)
{
	const char *position, *end;

	if (!*filter)
		return 1;
	for (position = filter; *position; position = *end ? end + 1 : end) {
		size_t length = 0;
		end = strchr(position, ',');
		if (!end)
			end = position + strlen(position);
		length = (size_t)(end - position);
		if ((strlen(candidate->ibdev) == length
		     && !strncmp(position, candidate->ibdev, length))
		 || (strlen(candidate->netdev) == length
		     && !strncmp(position, candidate->netdev, length))
		 || (strlen(candidate->address) == length
		     && !strncmp(position, candidate->address, length)))
			return 1;
		if (!*end)
			break;
	}
	return 0;
}

static int candidate_compare(const void *left, const void *right)
{
	const struct rp_candidate *a = left, *b = right;
	int result;
	if (a->rate_gbps != b->rate_gbps)
		return a->rate_gbps < b->rate_gbps ? 1 : -1;
	result = strcmp(a->ibdev, b->ibdev);
	return result ? result : strcmp(a->netdev, b->netdev);
}

static size_t enumerate_candidates(struct rp_session *session)
{
	DIR *ib_directory, *net_directory;
	struct dirent *ib_entry, *net_entry;
	char path[PATH_MAX], value[128];
	size_t count = 0;

	if (!(ib_directory = opendir("/sys/class/infiniband"))) {
		rp_set_error("cannot enumerate /sys/class/infiniband: %s", strerror(errno));
		return 0;
	}
	while (count < RP_MAX_ENDPOINTS && (ib_entry = readdir(ib_directory))) {
		if (ib_entry->d_name[0] == '.')
			continue;
		snprintf(path, sizeof path, "/sys/class/infiniband/%s/ports/1/state",
			 ib_entry->d_name);
		if (read_text_file(path, value, sizeof value) < 0 || strncmp(value, "4:", 2))
			continue;
		snprintf(path, sizeof path, "/sys/class/infiniband/%s/ports/1/link_layer",
			 ib_entry->d_name);
		if (read_text_file(path, value, sizeof value) < 0 || strcmp(value, "Ethernet"))
			continue;
		snprintf(path, sizeof path, "/sys/class/infiniband/%s/device/net",
			 ib_entry->d_name);
		if (!(net_directory = opendir(path)))
			continue;
		while (count < RP_MAX_ENDPOINTS && (net_entry = readdir(net_directory))) {
			struct rp_candidate candidate;

			if (net_entry->d_name[0] == '.')
				continue;
			if (strlen(ib_entry->d_name) >= sizeof candidate.ibdev
			 || strlen(net_entry->d_name) >= sizeof candidate.netdev)
				continue;
			memset(&candidate, 0, sizeof candidate);
			candidate.listen_fd = -1;
			candidate.port_num = 1;
			memcpy(candidate.ibdev, ib_entry->d_name, strlen(ib_entry->d_name) + 1);
			memcpy(candidate.netdev, net_entry->d_name, strlen(net_entry->d_name) + 1);
			if (!find_ipv4(candidate.netdev, &candidate.sin, candidate.address,
				       sizeof candidate.address)
			 || !find_gid(candidate.ibdev, candidate.netdev,
				      &candidate.sin.sin_addr, &candidate.gid_index, &candidate.gid)
			 || !filter_matches(session->config->device_filter, &candidate))
				continue;
			snprintf(path, sizeof path, "/sys/class/infiniband/%s/ports/1/rate",
				 ib_entry->d_name);
			if (!read_text_file(path, value, sizeof value))
				candidate.rate_gbps = (uint32_t)strtoul(value, NULL, 10);
			session->candidates[count++] = candidate;
		}
		closedir(net_directory);
	}
	closedir(ib_directory);
	qsort(session->candidates, count, sizeof session->candidates[0], candidate_compare);
	if (!count)
		rp_set_error("no active IPv4 RoCE-v2 endpoint matches the local selection");
	return count;
}

static int make_listener(struct rp_session *session, size_t index)
{
	struct rp_candidate *candidate = &session->candidates[index];
	struct sockaddr_in address = candidate->sin;
	socklen_t address_size = sizeof address;
	uint32_t requested = session->config->port
		? session->config->port + (uint32_t)index : 0;
	int fd, one = 1;

	if (requested > 65535) {
		rp_set_error("bootstrap port range exceeds 65535");
		return -1;
	}
	if ((fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0)) < 0) {
		rp_set_error("cannot create bootstrap listener: %s", strerror(errno));
		return -1;
	}
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
	address.sin_port = htons((uint16_t)requested);
	if (bind(fd, (struct sockaddr *)&address, sizeof address) < 0
	 || listen(fd, (int)RP_MAX_CHANNELS) < 0
	 || getsockname(fd, (struct sockaddr *)&address, &address_size) < 0) {
		rp_set_error("cannot listen on %s: %s", candidate->address, strerror(errno));
		close(fd);
		return -1;
	}
	candidate->listen_fd = fd;
	candidate->listen_port = ntohs(address.sin_port);
	return 0;
}

static void close_listeners(struct rp_session *session)
{
	size_t i;
	for (i = 0; i < session->candidate_count; i++) {
		if (session->candidates[i].listen_fd >= 0) {
			close(session->candidates[i].listen_fd);
			session->candidates[i].listen_fd = -1;
		}
	}
}

static struct ibv_context *open_verbs_device(const char *name)
{
	struct ibv_device **devices;
	struct ibv_context *context = NULL;
	int count, i;

	devices = ibv_get_device_list(&count);
	if (!devices)
		return NULL;
	for (i = 0; i < count; i++) {
		if (!strcmp(ibv_get_device_name(devices[i]), name)) {
			context = ibv_open_device(devices[i]);
			break;
		}
	}
	ibv_free_device_list(devices);
	return context;
}

static void destroy_path(struct rp_path *path)
{
	if (path->qp)
		ibv_destroy_qp(path->qp);
	if (path->mr)
		ibv_dereg_mr(path->mr);
	if (path->cq)
		ibv_destroy_cq(path->cq);
	if (path->pd)
		ibv_dealloc_pd(path->pd);
	if (path->context)
		ibv_close_device(path->context);
	free(path->ring);
	memset(path, 0, sizeof *path);
}

static void destroy_session(struct rp_session *session)
{
	unsigned i;
	for (i = 0; i < RP_MAX_CHANNELS; i++)
		destroy_path(&session->paths[i]);
	close_listeners(session);
	if (session->control_fd >= 0) {
		close(session->control_fd);
		session->control_fd = -1;
	}
}

static int post_receive(struct rp_path *path, uint32_t slot)
{
	struct ibv_sge sge = {
		.addr = (uintptr_t)(path->ring + path->stride * slot),
		.length = (uint32_t)path->stride,
		.lkey = path->mr->lkey
	};
	struct ibv_recv_wr request = {
		.wr_id = slot,
		.sg_list = &sge,
		.num_sge = 1
	};
	struct ibv_recv_wr *bad;
	return ibv_post_recv(path->qp, &request, &bad);
}

static int modify_qp_init(struct rp_path *path)
{
	struct ibv_qp_attr attributes;
	memset(&attributes, 0, sizeof attributes);
	attributes.qp_state = IBV_QPS_INIT;
	attributes.pkey_index = 0;
	attributes.port_num = (uint8_t)path->candidate.port_num;
	return ibv_modify_qp(path->qp, &attributes,
		IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS);
}

static int modify_qp_ready(struct rp_path *path, const struct rp_bootstrap *remote,
			   uint32_t local_psn)
{
	struct ibv_qp_attr attributes;
	struct ibv_port_attr port_attributes;

	if (ibv_query_port(path->context, (uint8_t)path->candidate.port_num,
			   &port_attributes))
		return -1;
	memset(&attributes, 0, sizeof attributes);
	attributes.qp_state = IBV_QPS_RTR;
	attributes.path_mtu = port_attributes.active_mtu < (enum ibv_mtu)remote->mtu
		? port_attributes.active_mtu : (enum ibv_mtu)remote->mtu;
	attributes.dest_qp_num = remote->qpn;
	attributes.rq_psn = remote->psn;
	attributes.max_dest_rd_atomic = 1;
	attributes.min_rnr_timer = 12;
	attributes.ah_attr.is_global = 1;
	attributes.ah_attr.port_num = (uint8_t)path->candidate.port_num;
	attributes.ah_attr.grh.dgid = remote->gid;
	attributes.ah_attr.grh.sgid_index = (uint8_t)path->candidate.gid_index;
	attributes.ah_attr.grh.hop_limit = 64;
	if (ibv_modify_qp(path->qp, &attributes, IBV_QP_STATE | IBV_QP_AV
		| IBV_QP_PATH_MTU | IBV_QP_DEST_QPN | IBV_QP_RQ_PSN
		| IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER))
		return -1;
	memset(&attributes, 0, sizeof attributes);
	attributes.qp_state = IBV_QPS_RTS;
	attributes.timeout = 14;
	attributes.retry_cnt = 7;
	attributes.rnr_retry = 7;
	attributes.sq_psn = local_psn;
	attributes.max_rd_atomic = 1;
	return ibv_modify_qp(path->qp, &attributes, IBV_QP_STATE | IBV_QP_TIMEOUT
		| IBV_QP_RETRY_CNT | IBV_QP_RNR_RETRY | IBV_QP_SQ_PSN
		| IBV_QP_MAX_QP_RD_ATOMIC);
}

static int setup_path(struct rp_session *session, struct rp_path *path,
		      const struct rp_candidate *candidate)
{
	struct ibv_device_attr device_attributes;
	struct ibv_qp_init_attr initial;
	size_t total;
	uint32_t i;

	memset(path, 0, sizeof *path);
	path->candidate = *candidate;
	path->candidate.listen_fd = -1;
	path->depth = session->config->queue_depth;
	path->stride = (sizeof(struct rp_frame_header) + session->config->chunk_size + 63U)
		& ~(size_t)63U;
	if (path->depth > SIZE_MAX / path->stride) {
		rp_set_error("registered ring size overflows size_t");
		return -1;
	}
	total = path->stride * path->depth;
	if (posix_memalign((void **)&path->ring, 4096, total)) {
		rp_set_error("cannot allocate %zu-byte registered ring", total);
		return -1;
	}
	memset(path->ring, 0, total);
	path->context = open_verbs_device(candidate->ibdev);
	if (!path->context || ibv_query_device(path->context, &device_attributes)
	 || path->depth > (uint32_t)device_attributes.max_qp_wr
	 || !(path->pd = ibv_alloc_pd(path->context))
	 || !(path->cq = ibv_create_cq(path->context, (int)path->depth * 2,
					NULL, NULL, 0))) {
		rp_set_error("cannot allocate verbs resources on %s", candidate->ibdev);
		return -1;
	}
	memset(&initial, 0, sizeof initial);
	initial.send_cq = initial.recv_cq = path->cq;
	initial.qp_type = IBV_QPT_RC;
	initial.cap.max_send_wr = path->depth;
	initial.cap.max_recv_wr = path->depth;
	initial.cap.max_send_sge = initial.cap.max_recv_sge = 1;
	if (!(path->qp = ibv_create_qp(path->pd, &initial))
	 || !(path->mr = ibv_reg_mr(path->pd, path->ring, total, IBV_ACCESS_LOCAL_WRITE))
	 || modify_qp_init(path)) {
		rp_set_error("cannot create/register RC queue on %s", candidate->ibdev);
		return -1;
	}
	if (!session->sender) {
		for (i = 0; i < path->depth; i++) {
			if (post_receive(path, i)) {
				rp_set_error("cannot post receive ring on %s", candidate->ibdev);
				return -1;
			}
		}
	}
	return 0;
}

static void put_u32(unsigned char **position, uint32_t value)
{
	value = htonl(value);
	memcpy(*position, &value, sizeof value);
	*position += sizeof value;
}

static uint32_t get_u32(const unsigned char **position)
{
	uint32_t value;
	memcpy(&value, *position, sizeof value);
	*position += sizeof value;
	return ntohl(value);
}

static void encode_bootstrap(unsigned char output[RP_BOOTSTRAP_SIZE],
			     const struct rp_bootstrap *bootstrap)
{
	unsigned char *position = output;
	put_u32(&position, RP_CONTROL_MAGIC);
	put_u32(&position, RP_PROTOCOL_VERSION);
	put_u32(&position, bootstrap->channel);
	put_u32(&position, bootstrap->channels);
	put_u32(&position, bootstrap->endpoint);
	put_u32(&position, bootstrap->chunk_size);
	put_u32(&position, bootstrap->queue_depth);
	put_u32(&position, bootstrap->qpn);
	put_u32(&position, bootstrap->psn);
	put_u32(&position, bootstrap->mtu);
	memcpy(position, bootstrap->gid.raw, 16);
	position += 16;
	memcpy(position, bootstrap->token, 16);
}

static int decode_bootstrap(const unsigned char input[RP_BOOTSTRAP_SIZE],
			    struct rp_bootstrap *bootstrap)
{
	const unsigned char *position = input;
	if (get_u32(&position) != RP_CONTROL_MAGIC
	 || get_u32(&position) != RP_PROTOCOL_VERSION)
		return -1;
	bootstrap->channel = get_u32(&position);
	bootstrap->channels = get_u32(&position);
	bootstrap->endpoint = get_u32(&position);
	bootstrap->chunk_size = get_u32(&position);
	bootstrap->queue_depth = get_u32(&position);
	bootstrap->qpn = get_u32(&position);
	bootstrap->psn = get_u32(&position);
	bootstrap->mtu = get_u32(&position);
	memcpy(bootstrap->gid.raw, position, 16);
	position += 16;
	memcpy(bootstrap->token, position, 16);
	return 0;
}

static int local_bootstrap(struct rp_session *session, struct rp_path *path,
			   uint32_t channel, uint32_t endpoint,
			   struct rp_bootstrap *bootstrap)
{
	struct ibv_port_attr attributes;
	if (ibv_query_port(path->context, (uint8_t)path->candidate.port_num, &attributes))
		return -1;
	memset(bootstrap, 0, sizeof *bootstrap);
	bootstrap->channel = channel;
	bootstrap->channels = session->channel_count;
	bootstrap->endpoint = endpoint;
	bootstrap->chunk_size = session->config->chunk_size;
	bootstrap->queue_depth = session->config->queue_depth;
	bootstrap->qpn = path->qp->qp_num;
	bootstrap->psn = ((uint32_t)session->token[channel * 4] << 16)
		| ((uint32_t)session->token[channel * 4 + 1] << 8)
		| session->token[channel * 4 + 2];
	bootstrap->mtu = (uint32_t)attributes.active_mtu;
	bootstrap->gid = path->candidate.gid;
	memcpy(bootstrap->token, session->token, 16);
	return 0;
}

static void encode_frame(struct rp_frame_header *header, uint32_t type,
			 uint32_t length, uint64_t sequence)
{
	header->magic = htonl(RP_FRAME_MAGIC);
	header->type = htonl(type);
	header->length = htonl(length);
	header->reserved = 0;
	header->sequence = rp_hton64(sequence);
}

static int poll_completion(struct rp_session *session, struct rp_path *path,
			   struct ibv_wc *completion)
{
	struct timespec start;
	int limit = timeout_ms(session);

	clock_gettime(CLOCK_MONOTONIC, &start);
	while (!rp_interrupted) {
		int result = ibv_poll_cq(path->cq, 1, completion);
		if (result > 0) {
			if (completion->status != IBV_WC_SUCCESS) {
				rp_set_error("completion failed on %s: %s",
					path->candidate.ibdev,
					ibv_wc_status_str(completion->status));
				return RP_EXIT_IO;
			}
			return 0;
		}
		if (result < 0) {
			rp_set_error("cannot poll completion queue on %s", path->candidate.ibdev);
			return RP_EXIT_IO;
		}
		if (!session->sender && session->control_fd >= 0
		 && wait_fd(session->control_fd, POLLIN, 0)) {
			rp_set_error("sender cancelled or closed the control channel");
			return RP_EXIT_IO;
		}
		if (elapsed_ms(&start) >= limit) {
			rp_set_error("RDMA channel on %s made no progress for %u seconds",
				path->candidate.ibdev, session->config->timeout);
			return RP_EXIT_TIMEOUT;
		}
	}
	rp_set_error("interrupted");
	return rp_interrupted_exit_code();
}

static int post_probe(struct rp_session *session, struct rp_path *path,
		      uint32_t channel)
{
	struct rp_frame_header *header = (struct rp_frame_header *)path->ring;
	struct ibv_sge sge;
	struct ibv_send_wr request, *bad;
	struct ibv_wc completion;

	encode_frame(header, RP_FRAME_PROBE, 0, channel);
	memset(&sge, 0, sizeof sge);
	sge.addr = (uintptr_t)header;
	sge.length = sizeof *header;
	sge.lkey = path->mr->lkey;
	memset(&request, 0, sizeof request);
	request.sg_list = &sge;
	request.num_sge = 1;
	request.opcode = IBV_WR_SEND;
	request.send_flags = IBV_SEND_SIGNALED;
	if (ibv_post_send(path->qp, &request, &bad)) {
		rp_set_error("cannot post fabric probe on %s", path->candidate.ibdev);
		return RP_EXIT_IO;
	}
	{
		int result = poll_completion(session, path, &completion);
		if (result)
			return result;
	}
	return completion.opcode == IBV_WC_SEND ? 0 : RP_EXIT_PROTOCOL;
}

static int receive_probe(struct rp_session *session, struct rp_path *path,
			 uint32_t channel)
{
	struct ibv_wc completion;
	struct rp_frame_header *header;
	int result = poll_completion(session, path, &completion);
	if (result)
		return result;
	if (completion.opcode != IBV_WC_RECV || completion.wr_id >= path->depth
	 || completion.byte_len != sizeof *header)
		goto invalid;
	header = (struct rp_frame_header *)(path->ring + path->stride * completion.wr_id);
	if (ntohl(header->magic) != RP_FRAME_MAGIC
	 || ntohl(header->type) != RP_FRAME_PROBE || ntohl(header->length)
	 || ntohl(header->reserved) || rp_ntoh64(header->sequence) != channel
	 || post_receive(path, (uint32_t)completion.wr_id))
		goto invalid;
	return 0;

invalid:
	rp_set_error("invalid fabric probe on %s", path->candidate.ibdev);
	return RP_EXIT_PROTOCOL;
}

static int exchange_ready(struct rp_session *session, int fd)
{
	unsigned char ready = 1, peer = 0;
	if (socket_write_all(fd, &ready, 1, timeout_ms(session))
	 || socket_read_all(fd, &peer, 1, timeout_ms(session))) {
		int error_number = errno;
		int result = control_io_exit_code(error_number);
		if (result == RP_EXIT_TIMEOUT)
			rp_set_error("RDMA ready exchange timed out");
		else if (rp_interrupted)
			rp_set_error("interrupted during RDMA ready exchange");
		else
			rp_set_error("RDMA ready exchange failed: %s", strerror(error_number));
		return result;
	}
	if (peer != 1) {
		rp_set_error("invalid RDMA ready response");
		return RP_EXIT_PROTOCOL;
	}
	return 0;
}

static int accept_any(struct rp_session *session, size_t *endpoint, int timeout)
{
	struct pollfd pollfds[RP_MAX_ENDPOINTS];
	struct timespec start;
	size_t i;

	clock_gettime(CLOCK_MONOTONIC, &start);
	while (!rp_interrupted) {
		int remaining = timeout - elapsed_ms(&start);
		int result;
		if (remaining <= 0)
			break;
		for (i = 0; i < session->candidate_count; i++) {
			pollfds[i].fd = session->candidates[i].listen_fd;
			pollfds[i].events = POLLIN;
			pollfds[i].revents = 0;
		}
		do {
			result = poll(pollfds, session->candidate_count, remaining);
		} while (result < 0 && errno == EINTR && !rp_interrupted);
		if (result <= 0)
			break;
		for (i = 0; i < session->candidate_count; i++) {
			int fd;
			if (!(pollfds[i].revents & POLLIN))
				continue;
			do {
				fd = accept4(pollfds[i].fd, NULL, NULL, SOCK_CLOEXEC);
			} while (fd < 0 && errno == EINTR);
			if (fd >= 0) {
				*endpoint = i;
				return fd;
			}
		}
	}
	rp_set_error(rp_interrupted ? "interrupted" : "timed out waiting for RDMA receiver");
	return -1;
}

static int connect_bootstrap(struct rp_session *session,
			     const struct rp_candidate *local,
			     const struct rp_endpoint *remote)
{
	struct sockaddr_in remote_address, local_address = local->sin;
	struct timespec start;
	int fd = -1, flags, result, error = 0;
	socklen_t error_size = sizeof error;

	fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0)
		goto failed;
	local_address.sin_port = 0;
	if (bind(fd, (struct sockaddr *)&local_address, sizeof local_address) < 0)
		goto failed;
	memset(&remote_address, 0, sizeof remote_address);
	remote_address.sin_family = AF_INET;
	remote_address.sin_port = htons(remote->port);
	if (inet_pton(AF_INET, remote->address, &remote_address.sin_addr) != 1) {
		errno = EINVAL;
		goto failed;
	}
	flags = fcntl(fd, F_GETFL, 0);
	if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
		goto failed;
	clock_gettime(CLOCK_MONOTONIC, &start);
	result = connect(fd, (struct sockaddr *)&remote_address, sizeof remote_address);
	if (result < 0 && errno != EINPROGRESS)
		goto failed;
	if (result < 0) {
		int remaining = timeout_ms(session) - elapsed_ms(&start);
		int ready;
		if (remaining <= 0) {
			errno = ETIMEDOUT;
			goto failed;
		}
		ready = wait_fd(fd, POLLOUT, remaining);
		if (!ready) {
			errno = ETIMEDOUT;
			goto failed;
		}
		if (ready < 0
		 || getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &error_size) < 0 || error) {
			errno = error ? error : errno;
			goto failed;
		}
	}
	if (fcntl(fd, F_SETFL, flags) < 0)
		goto failed;
	return fd;

failed:
	rp_set_error("cannot connect bootstrap %s:%u from %s: %s", remote->address,
		remote->port, local->address, strerror(errno));
	if (fd >= 0)
		close(fd);
	return -1;
}

static uint32_t choose_channels(const struct rp_descriptor *descriptor,
				const struct rp_session *session)
{
	uint32_t remote_request = session->config->channels;
	size_t local_count = session->candidate_count;
	size_t remote_count = descriptor->endpoint_count;

	if (descriptor->channels && remote_request
	 && descriptor->channels != remote_request)
		return 0;
	if (descriptor->channels)
		return descriptor->channels;
	if (remote_request)
		return remote_request;
	if (local_count >= 2 && remote_count >= 2)
		return 2;
	if (local_count == 1 && remote_count >= 2
	 && session->candidates[0].rate_gbps * 2U
		>= descriptor->endpoints[0].rate_gbps * 3U)
		return 2;
	if (local_count >= 2 && remote_count == 1
	 && descriptor->endpoints[0].rate_gbps * 2U
		>= session->candidates[0].rate_gbps * 3U)
		return 2;
	return 1;
}

static int setup_receiver_path(struct rp_session *session,
			       const struct rp_descriptor *descriptor,
			       uint32_t channel)
{
	uint32_t local_index = channel % (uint32_t)session->candidate_count;
	uint32_t remote_index = channel % (uint32_t)descriptor->endpoint_count;
	struct rp_path *path = &session->paths[channel];
	struct rp_bootstrap local, remote;
	unsigned char local_wire[RP_BOOTSTRAP_SIZE], remote_wire[RP_BOOTSTRAP_SIZE];
	int fd, result;

	if (setup_path(session, path, &session->candidates[local_index]) < 0)
		return RP_EXIT_UNAVAILABLE;
	fd = connect_bootstrap(session, &session->candidates[local_index],
			       &descriptor->endpoints[remote_index]);
	if (fd < 0)
		return control_io_exit_code(errno) == RP_EXIT_TIMEOUT
			? RP_EXIT_TIMEOUT : RP_EXIT_UNAVAILABLE;
	if (local_bootstrap(session, path, channel, remote_index, &local) < 0) {
		close(fd);
		return RP_EXIT_IO;
	}
	encode_bootstrap(local_wire, &local);
	if (socket_write_all(fd, local_wire, sizeof local_wire, timeout_ms(session))
	 || socket_read_all(fd, remote_wire, sizeof remote_wire, timeout_ms(session))) {
		int error_number = errno;
		result = control_io_exit_code(error_number);
		if (result == RP_EXIT_TIMEOUT)
			rp_set_error("QP bootstrap timed out for channel %u", channel + 1);
		else if (rp_interrupted)
			rp_set_error("interrupted during QP bootstrap");
		else
			rp_set_error("QP bootstrap I/O failed for channel %u: %s",
				channel + 1, strerror(error_number));
		close(fd);
		return result;
	}
	if (decode_bootstrap(remote_wire, &remote) < 0
	 || remote.channel != channel || remote.channels != session->channel_count
	 || remote.endpoint != remote_index
	 || remote.chunk_size != session->config->chunk_size
	 || remote.queue_depth != session->config->queue_depth
	 || !remote.qpn || remote.qpn > 0xffffffU || remote.psn > 0xffffffU
	 || remote.mtu < IBV_MTU_256 || remote.mtu > IBV_MTU_4096
	 || !token_equal(remote.token, session->token)) {
		rp_set_error("invalid QP bootstrap response for channel %u", channel + 1);
		close(fd);
		return RP_EXIT_PROTOCOL;
	}
	if (modify_qp_ready(path, &remote, local.psn)) {
		rp_set_error("cannot transition receiver QP on %s", path->candidate.ibdev);
		close(fd);
		return RP_EXIT_IO;
	}
	result = receive_probe(session, path, channel);
	if (!result)
		result = exchange_ready(session, fd);
	if (result) {
		close(fd);
		return result;
	}
	if (!channel)
		session->control_fd = fd;
	else
		close(fd);
	rp_log(session->config, 1, "channel %u: %s/%s <- %s", channel + 1,
		path->candidate.ibdev, path->candidate.address,
		descriptor->endpoints[remote_index].address);
	return 0;
}

static int setup_sender_path(struct rp_session *session, uint32_t channel)
{
	struct rp_bootstrap remote, local;
	unsigned char remote_wire[RP_BOOTSTRAP_SIZE], local_wire[RP_BOOTSTRAP_SIZE];
	struct rp_path *path = &session->paths[channel];
	struct timespec start;
	int fd = -1, result;
	size_t endpoint = 0;

	clock_gettime(CLOCK_MONOTONIC, &start);
	for (;;) {
		int remaining = timeout_ms(session) - elapsed_ms(&start);
		int valid;

		if (remaining <= 0) {
			rp_set_error("timed out waiting for authenticated receiver");
			return RP_EXIT_TIMEOUT;
		}
		fd = accept_any(session, &endpoint, remaining);
		if (fd < 0)
			return rp_interrupted ? rp_interrupted_exit_code() : RP_EXIT_TIMEOUT;
		remaining = timeout_ms(session) - elapsed_ms(&start);
		if (remaining <= 0) {
			close(fd);
			rp_set_error("timed out waiting for authenticated receiver");
			return RP_EXIT_TIMEOUT;
		}
		if (socket_read_all(fd, remote_wire, sizeof remote_wire, remaining)) {
			int io_result = control_io_exit_code(errno);
			close(fd);
			if (rp_interrupted)
				return io_result;
			if (io_result == RP_EXIT_TIMEOUT) {
				rp_set_error("timed out waiting for authenticated receiver");
				return RP_EXIT_TIMEOUT;
			}
			continue;
		}
		valid = !decode_bootstrap(remote_wire, &remote)
		 && token_equal(remote.token, session->token)
		 && remote.channel == channel && remote.endpoint == endpoint
		 && remote.chunk_size == session->config->chunk_size
		 && remote.queue_depth == session->config->queue_depth
		 && remote.qpn && remote.qpn <= 0xffffffU && remote.psn <= 0xffffffU
		 && remote.mtu >= IBV_MTU_256 && remote.mtu <= IBV_MTU_4096
		 && remote.channels >= 1 && remote.channels <= RP_MAX_CHANNELS
		 && (!session->config->channels || remote.channels == session->config->channels);
		if (valid)
			break;
		close(fd);
		if (elapsed_ms(&start) >= timeout_ms(session)) {
			rp_set_error("timed out waiting for authenticated receiver");
			return RP_EXIT_TIMEOUT;
		}
	}
	if (!channel)
		session->channel_count = remote.channels;
	else if (remote.channels != session->channel_count) {
		rp_set_error("receiver changed channel count during setup");
		close(fd);
		return RP_EXIT_PROTOCOL;
	}
	if (setup_path(session, path, &session->candidates[endpoint]) < 0
	 || local_bootstrap(session, path, channel, (uint32_t)endpoint, &local) < 0) {
		close(fd);
		return RP_EXIT_UNAVAILABLE;
	}
	encode_bootstrap(local_wire, &local);
	if (socket_write_all(fd, local_wire, sizeof local_wire, timeout_ms(session))) {
		int error_number = errno;
		result = control_io_exit_code(error_number);
		if (result == RP_EXIT_TIMEOUT)
			rp_set_error("sender QP bootstrap timed out on %s", path->candidate.ibdev);
		else if (rp_interrupted)
			rp_set_error("interrupted during sender QP bootstrap");
		else
			rp_set_error("cannot write sender QP bootstrap on %s: %s",
				path->candidate.ibdev, strerror(error_number));
		close(fd);
		return result;
	}
	if (modify_qp_ready(path, &remote, local.psn)) {
		rp_set_error("cannot transition sender QP on %s", path->candidate.ibdev);
		close(fd);
		return RP_EXIT_IO;
	}
	result = post_probe(session, path, channel);
	if (!result)
		result = exchange_ready(session, fd);
	if (result) {
		close(fd);
		return result;
	}
	if (!channel)
		session->control_fd = fd;
	else
		close(fd);
	rp_log(session->config, 1, "channel %u: %s/%s -> receiver", channel + 1,
		path->candidate.ibdev, path->candidate.address);
	return 0;
}

static void encode_status(unsigned char output[RP_STATUS_SIZE],
			  const struct rp_status *status)
{
	unsigned char *position = output;
	uint64_t number;
	memset(output, 0, RP_STATUS_SIZE);
	put_u32(&position, RP_CONTROL_MAGIC);
	put_u32(&position, RP_PROTOCOL_VERSION);
	put_u32(&position, status->state);
	put_u32(&position, status->code);
	number = rp_hton64(status->bytes);
	memcpy(position, &number, 8);
	position += 8;
	number = rp_hton64(status->messages);
	memcpy(position, &number, 8);
	position += 8;
	memcpy(position, status->token, 16);
	position += 16;
	memcpy(position, status->reason, 48);
}

static int decode_status(const unsigned char input[RP_STATUS_SIZE],
			 struct rp_status *status)
{
	const unsigned char *position = input;
	uint64_t number;
	if (get_u32(&position) != RP_CONTROL_MAGIC
	 || get_u32(&position) != RP_PROTOCOL_VERSION)
		return -1;
	status->state = get_u32(&position);
	status->code = get_u32(&position);
	memcpy(&number, position, 8);
	position += 8;
	status->bytes = rp_ntoh64(number);
	memcpy(&number, position, 8);
	position += 8;
	status->messages = rp_ntoh64(number);
	memcpy(status->token, position, 16);
	position += 16;
	memcpy(status->reason, position, 48);
	status->reason[48] = '\0';
	return status->state <= RP_STATUS_CANCEL && status->code <= 255 ? 0 : -1;
}

static int send_status(struct rp_session *session, uint32_t state, uint32_t code,
		       const char *reason)
{
	struct rp_status status;
	unsigned char wire[RP_STATUS_SIZE];
	memset(&status, 0, sizeof status);
	status.state = state;
	status.code = code;
	status.bytes = session->bytes;
	status.messages = session->messages;
	memcpy(status.token, session->token, 16);
	if (reason)
		snprintf(status.reason, sizeof status.reason, "%s", reason);
	encode_status(wire, &status);
	return session->control_fd < 0 ? -1
		: socket_write_all(session->control_fd, wire, sizeof wire, timeout_ms(session));
}

static int receive_status(struct rp_session *session, struct rp_status *status)
{
	unsigned char wire[RP_STATUS_SIZE];
	if (socket_read_all(session->control_fd, wire, sizeof wire, timeout_ms(session))) {
		int error_number = errno;
		int result = control_io_exit_code(error_number);
		if (result == RP_EXIT_TIMEOUT)
			rp_set_error("timed out waiting for final receiver status");
		else if (rp_interrupted)
			rp_set_error("interrupted while waiting for final receiver status");
		else
			rp_set_error("cannot read final receiver status: %s", strerror(error_number));
		return result;
	}
	if (decode_status(wire, status) < 0
	 || !token_equal(status->token, session->token)) {
		rp_set_error("invalid or missing final receiver status");
		return RP_EXIT_PROTOCOL;
	}
	return 0;
}

static int wait_send_slot(struct rp_session *session, struct rp_path *path)
{
	struct ibv_wc completion;
	int result;
	if (path->outstanding < path->depth)
		return 0;
	result = poll_completion(session, path, &completion);
	if (result)
		return result;
	if (completion.opcode != IBV_WC_SEND) {
		rp_set_error("unexpected completion on sender channel");
		return RP_EXIT_PROTOCOL;
	}
	path->outstanding--;
	return 0;
}

static int post_send_frame(struct rp_path *path,
			   uint32_t slot, uint32_t type, uint32_t length,
			   uint64_t sequence)
{
	struct rp_frame_header *header = (struct rp_frame_header *)
		(path->ring + path->stride * slot);
	struct ibv_sge sge;
	struct ibv_send_wr request, *bad;

	encode_frame(header, type, length, sequence);
	memset(&sge, 0, sizeof sge);
	sge.addr = (uintptr_t)header;
	sge.length = (uint32_t)sizeof *header + length;
	sge.lkey = path->mr->lkey;
	memset(&request, 0, sizeof request);
	request.wr_id = slot;
	request.sg_list = &sge;
	request.num_sge = 1;
	request.opcode = IBV_WR_SEND;
	request.send_flags = IBV_SEND_SIGNALED;
	if (ibv_post_send(path->qp, &request, &bad)) {
		rp_set_error("cannot post RDMA frame on %s: %s",
			path->candidate.ibdev, strerror(errno));
		return RP_EXIT_IO;
	}
	path->outstanding++;
	return 0;
}

static int read_input_chunk(struct rp_session *session, int input_fd,
			    unsigned char *output, size_t capacity, size_t *size)
{
	size_t used = 0;

	while (!rp_interrupted && used < capacity) {
		struct pollfd pollfds[2] = {
			{ .fd = input_fd, .events = POLLIN },
			{ .fd = session->control_fd, .events = POLLIN }
		};
		int delay = used ? 1 : timeout_ms(session);
		int result;

		do {
			result = poll(pollfds, 2, delay);
		} while (result < 0 && errno == EINTR && !rp_interrupted);
		if (result < 0) {
			rp_set_error("cannot poll input: %s", strerror(errno));
			return RP_EXIT_IO;
		}
		if (!result) {
			if (used)
				break;
			rp_set_error("producer made no progress for %u seconds",
				session->config->timeout);
			return RP_EXIT_TIMEOUT;
		}
		if (pollfds[1].revents & (POLLIN | POLLHUP | POLLERR)) {
			rp_set_error("receiver failed before sender EOF");
			return RP_EXIT_IO;
		}
		if (!(pollfds[0].revents & (POLLIN | POLLHUP)))
			continue;
		for (;;) {
			ssize_t amount = read(input_fd, output + used, capacity - used);
			if (amount < 0 && errno == EINTR)
				continue;
			if (amount < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
				break;
			if (amount < 0) {
				rp_set_error("cannot read stdin: %s", strerror(errno));
				return RP_EXIT_IO;
			}
			if (!amount) {
				*size = used;
				return 1;
			}
			used += (size_t)amount;
			if (used == capacity || !session->input_nonblocking)
				break;
		}
	}
	if (rp_interrupted) {
		rp_set_error("interrupted");
		return rp_interrupted_exit_code();
	}
	*size = used;
	return 0;
}

static int drain_sends(struct rp_session *session)
{
	uint32_t channel;
	for (channel = 0; channel < session->channel_count; channel++) {
		struct rp_path *path = &session->paths[channel];
		while (path->outstanding) {
			struct ibv_wc completion;
			int result = poll_completion(session, path, &completion);
			if (result)
				return result;
			if (completion.opcode != IBV_WC_SEND) {
				rp_set_error("unexpected sender completion while draining");
				return RP_EXIT_PROTOCOL;
			}
			path->outstanding--;
		}
	}
	return 0;
}

static int transfer_send(struct rp_session *session, int input_fd)
{
	uint64_t sequence = 0;
	int result;

	for (;;) {
		struct rp_path *path = &session->paths[sequence % session->channel_count];
		uint32_t slot;
		size_t length = 0;

		if ((result = wait_send_slot(session, path)))
			return result;
		result = read_input_chunk(session, input_fd,
			path->ring + path->stride * (path->next_slot % path->depth)
				+ sizeof(struct rp_frame_header),
			session->config->chunk_size, &length);
		if (result != 0 && result != 1)
			return result;
		if (length) {
			timer_start(session);
			slot = path->next_slot++ % path->depth;
			if ((result = post_send_frame(path, slot, RP_FRAME_DATA,
						  (uint32_t)length, sequence)))
				return result;
			session->bytes += length;
			session->messages++;
			sequence++;
		}
		if (result == 1)
			break;
	}
	{
		struct rp_path *path = &session->paths[sequence % session->channel_count];
		uint32_t slot;
		if ((result = wait_send_slot(session, path)))
			return result;
		slot = path->next_slot++ % path->depth;
		if ((result = post_send_frame(path, slot, RP_FRAME_FIN, 0, sequence)))
			return result;
	}
	if ((result = drain_sends(session)))
		return result;
	{
		struct rp_status status = { 0 };
		result = receive_status(session, &status);
		if (result)
			return result;
		if (status.bytes != session->bytes || status.messages != session->messages) {
			rp_set_error("receiver counters do not match sender counters");
			return RP_EXIT_PROTOCOL;
		}
		if (status.state != RP_STATUS_OK) {
			rp_set_error("receiver failed: %s", status.reason[0] ? status.reason : "remote error");
			return status.code ? (int)status.code : RP_EXIT_IO;
		}
	}
	clock_gettime(CLOCK_MONOTONIC, &session->finished);
	return 0;
}

static int write_output(struct rp_session *session, int output_fd,
			const unsigned char *data, size_t size)
{
	struct timespec start;
	clock_gettime(CLOCK_MONOTONIC, &start);
	while (size && !rp_interrupted) {
		struct pollfd pollfds[2] = {
			{ .fd = output_fd, .events = POLLOUT },
			{ .fd = session->control_fd, .events = POLLIN }
		};
		int remaining = timeout_ms(session) - elapsed_ms(&start);
		int result;
		ssize_t amount;
		if (remaining <= 0) {
			rp_set_error("consumer made no progress for %u seconds",
				session->config->timeout);
			return RP_EXIT_TIMEOUT;
		}
		do {
			result = poll(pollfds, 2, remaining);
		} while (result < 0 && errno == EINTR && !rp_interrupted);
		if (result <= 0) {
			rp_set_error(result ? "cannot poll consumer output: %s"
				: "consumer output timed out", result ? strerror(errno) : "");
			return result ? RP_EXIT_IO : RP_EXIT_TIMEOUT;
		}
		if (pollfds[1].revents & (POLLIN | POLLHUP | POLLERR)) {
			rp_set_error("sender cancelled or closed the control channel");
			return RP_EXIT_IO;
		}
		if (!(pollfds[0].revents & (POLLOUT | POLLHUP | POLLERR)))
			continue;
		amount = write(output_fd, data, size);
		if (amount < 0 && errno == EINTR)
			continue;
		if (amount <= 0) {
			rp_set_error("consumer closed its input: %s",
				amount < 0 ? strerror(errno) : "short write");
			return RP_EXIT_IO;
		}
		data += amount;
		size -= (size_t)amount;
		clock_gettime(CLOCK_MONOTONIC, &start);
	}
	if (rp_interrupted) {
		rp_set_error("interrupted");
		return rp_interrupted_exit_code();
	}
	return 0;
}

static int consumer_status(int status)
{
	if (WIFEXITED(status))
		return WEXITSTATUS(status);
	if (WIFSIGNALED(status))
		return 128 + WTERMSIG(status);
	return RP_EXIT_IO;
}

static void signal_consumer_tree(pid_t consumer, int signal_number)
{
	if (kill(-consumer, signal_number) < 0 && errno == ESRCH)
		kill(consumer, signal_number);
}

static int wait_consumer(pid_t consumer, int cancel_signal)
{
	int status, child_result = RP_EXIT_IO;
	struct timespec cancellation_started = { 0, 0 };
	int cancelling = cancel_signal > 0;
	int reaped = 0;

	if (consumer < 0)
		return 0;
	if (!cancelling && rp_interrupted) {
		cancel_signal = rp_interrupted > 0 && rp_interrupted < 128
			? (int)rp_interrupted : SIGINT;
		cancelling = 1;
	}
	if (cancelling) {
		signal_consumer_tree(consumer, cancel_signal);
		clock_gettime(CLOCK_MONOTONIC, &cancellation_started);
	}
	for (;;) {
		pid_t result = reaped ? 0
			: waitpid(consumer, &status, cancelling ? WNOHANG : 0);
		if (result == consumer) {
			child_result = consumer_status(status);
			reaped = 1;
			if (!cancelling)
				return child_result;
		}
		if (result < 0 && errno == EINTR) {
			if (rp_interrupted && !cancelling) {
				cancel_signal = rp_interrupted > 0 && rp_interrupted < 128
					? (int)rp_interrupted : SIGINT;
				cancelling = 1;
				signal_consumer_tree(consumer, cancel_signal);
				clock_gettime(CLOCK_MONOTONIC, &cancellation_started);
			}
			continue;
		}
		if (result < 0) {
			rp_set_error("cannot wait for remote consumer: %s", strerror(errno));
			return RP_EXIT_IO;
		}
		if (reaped && kill(-consumer, 0) < 0 && errno == ESRCH)
			return child_result;
		if (elapsed_ms(&cancellation_started) >= 2000) {
			signal_consumer_tree(consumer, SIGKILL);
			if (!reaped) {
				do {
					result = waitpid(consumer, &status, 0);
				} while (result < 0 && errno == EINTR);
				if (result != consumer) {
					rp_set_error("cannot reap remote consumer: %s",
						strerror(errno));
					return RP_EXIT_IO;
				}
				child_result = consumer_status(status);
			}
			return child_result;
		}
		poll(NULL, 0, 10);
	}
}

static int transfer_receive(struct rp_session *session, int output_fd,
			    pid_t consumer_pid)
{
	uint64_t sequence = 0;
	int output_flags = -1;
	int result = 0;

	if (!session->config->discard) {
		output_flags = fcntl(output_fd, F_GETFL, 0);
		if (output_flags < 0 || fcntl(output_fd, F_SETFL, output_flags | O_NONBLOCK) < 0) {
			rp_set_error("cannot make consumer output nonblocking: %s", strerror(errno));
			result = RP_EXIT_IO;
		}
	}

	while (!result) {
		struct rp_path *path = &session->paths[sequence % session->channel_count];
		struct ibv_wc completion;
		struct rp_frame_header *header;
		uint32_t type, length, slot;

		result = poll_completion(session, path, &completion);
		if (result)
			break;
		if (completion.opcode != IBV_WC_RECV || completion.wr_id >= path->depth
		 || completion.byte_len < sizeof *header) {
			rp_set_error("invalid receive completion on channel %u",
				(unsigned)(sequence % session->channel_count) + 1);
			result = RP_EXIT_PROTOCOL;
			break;
		}
		slot = (uint32_t)completion.wr_id;
		header = (struct rp_frame_header *)(path->ring + path->stride * slot);
		type = ntohl(header->type);
		length = ntohl(header->length);
		if (ntohl(header->magic) != RP_FRAME_MAGIC || ntohl(header->reserved)
		 || rp_ntoh64(header->sequence) != sequence
		 || length > session->config->chunk_size
		 || completion.byte_len != sizeof *header + length
		 || (type != RP_FRAME_DATA && type != RP_FRAME_FIN)
		 || (type == RP_FRAME_FIN && length)) {
			rp_set_error("invalid frame at sequence %llu",
				(unsigned long long)sequence);
			result = RP_EXIT_PROTOCOL;
			break;
		}
		if (type == RP_FRAME_FIN) {
			post_receive(path, slot);
			break;
		}
		timer_start(session);
		if (!session->config->discard)
			result = write_output(session, output_fd,
				(unsigned char *)(header + 1), length);
		if (result)
			break;
		session->bytes += length;
		session->messages++;
		if (post_receive(path, slot)) {
			rp_set_error("cannot repost receive slot on %s", path->candidate.ibdev);
			result = RP_EXIT_IO;
			break;
		}
		sequence++;
	}
	if (consumer_pid < 0 && output_flags >= 0)
		fcntl(output_fd, F_SETFL, output_flags);
	if (consumer_pid >= 0)
		close(output_fd);
	if (consumer_pid >= 0) {
		int consumer_result;
		int cancel_signal = result ? SIGTERM : 0;
		if (rp_interrupted)
			cancel_signal = rp_interrupted > 0 && rp_interrupted < 128
				? (int)rp_interrupted : SIGINT;
		consumer_result = wait_consumer(consumer_pid, cancel_signal);
		if (!result) {
			result = consumer_result;
			if (result)
				rp_set_error("remote consumer exited with status %d", result);
		} else if (consumer_result > 0 && consumer_result < 128) {
			result = consumer_result;
			rp_set_error("remote consumer exited with status %d", result);
		}
	}
	clock_gettime(CLOCK_MONOTONIC, &session->finished);
	if (result)
		send_status(session, result >= 128 ? RP_STATUS_CANCEL : RP_STATUS_ERROR,
			(uint32_t)(result > 255 ? RP_EXIT_IO : result), rp_last_error());
	else if (send_status(session, RP_STATUS_OK, 0, "") < 0) {
		rp_set_error("cannot send final stream status");
		result = RP_EXIT_IO;
	}
	return result;
}

static void show_summary(const struct rp_session *session, const char *direction,
			 int result)
{
	double seconds = 0, gbps = 0;
	uint64_t memory = (uint64_t)session->channel_count
		* session->config->queue_depth
		* ((sizeof(struct rp_frame_header) + session->config->chunk_size + 63U) & ~(size_t)63U);
	if (session->timer_started) {
		seconds = (double)(session->finished.tv_sec - session->started.tv_sec)
			+ (double)(session->finished.tv_nsec - session->started.tv_nsec)
			  / 1000000000.0;
		gbps = seconds > 0 ? (double)session->bytes * 8.0 / seconds / 1000000000.0 : 0;
	}
	rp_log(session->config, 0,
		"%s %s: %llu bytes, %llu messages, %.6f s, %.2f Gb/s; %u channel%s, %u-byte chunks, depth %u, %llu registered bytes",
		direction, result ? "failed" : "complete",
		(unsigned long long)session->bytes,
		(unsigned long long)session->messages, seconds, gbps,
		session->channel_count, session->channel_count == 1 ? "" : "s",
		session->config->chunk_size, session->config->queue_depth,
		(unsigned long long)memory);
}

int rp_send_stream(struct rp_config *config, int input_fd, int descriptor_fd)
{
	struct rp_session session;
	struct rp_descriptor descriptor;
	char descriptor_text[RP_MAX_DESCRIPTOR + 1];
	size_t usable = 0, i;
	int input_flags = -1;
	int length, result;

	memset(&session, 0, sizeof session);
	session.config = config;
	session.sender = 1;
	session.control_fd = -1;
	session.candidate_count = enumerate_candidates(&session);
	if (!session.candidate_count)
		return RP_EXIT_UNAVAILABLE;
	for (i = 0; i < session.candidate_count; i++) {
		if (!make_listener(&session, i)) {
			if (usable != i)
				session.candidates[usable] = session.candidates[i];
			usable++;
		}
	}
	session.candidate_count = usable;
	if (!usable) {
		destroy_session(&session);
		return RP_EXIT_UNAVAILABLE;
	}
	if ((result = rp_random_token(session.token))) {
		destroy_session(&session);
		return result;
	}
	memset(&descriptor, 0, sizeof descriptor);
	descriptor.version = RP_PROTOCOL_VERSION;
	memcpy(descriptor.token, session.token, 16);
	descriptor.chunk_size = config->chunk_size;
	descriptor.queue_depth = config->queue_depth;
	descriptor.channels = config->channels;
	descriptor.endpoint_count = session.candidate_count;
	for (i = 0; i < session.candidate_count; i++) {
		snprintf(descriptor.endpoints[i].address,
			sizeof descriptor.endpoints[i].address, "%s",
			session.candidates[i].address);
		descriptor.endpoints[i].port = session.candidates[i].listen_port;
		descriptor.endpoints[i].rate_gbps = session.candidates[i].rate_gbps;
	}
	length = rp_descriptor_format(&descriptor, descriptor_text, sizeof descriptor_text);
	if (length < 0 || rp_write_all(descriptor_fd, descriptor_text, (size_t)length) < 0) {
		rp_set_error("cannot write rendezvous descriptor: %s", strerror(errno));
		destroy_session(&session);
		return RP_EXIT_IO;
	}
	for (i = 0; i < RP_MAX_CHANNELS; i++) {
		result = setup_sender_path(&session, (uint32_t)i);
		if (result)
			goto done;
		if (i + 1 >= session.channel_count)
			break;
	}
	close_listeners(&session);
	rp_log(config, 0, "RDMA active: %u channel%s", session.channel_count,
		session.channel_count == 1 ? "" : "s");
	input_flags = prepare_input(&session, input_fd);
	result = transfer_send(&session, input_fd);
	{
		int restore_result = restore_input(&session, input_fd, input_flags);
		if (!result)
			result = restore_result;
	}
	if (result && session.control_fd >= 0)
		send_status(&session, rp_interrupted ? RP_STATUS_CANCEL : RP_STATUS_ERROR,
			(uint32_t)(result > 255 ? RP_EXIT_IO : result), rp_last_error());

done:
	if (session.timer_started && !session.finished.tv_sec)
		clock_gettime(CLOCK_MONOTONIC, &session.finished);
	if (session.channel_count)
		show_summary(&session, "send", result);
	destroy_session(&session);
	return result;
}

int rp_receive_stream(struct rp_config *config, int descriptor_fd, int output_fd,
			 pid_t consumer_pid)
{
	struct rp_session session;
	struct rp_descriptor descriptor;
	uint32_t channel;
	int stream_called = 0;
	int result;

	memset(&session, 0, sizeof session);
	session.config = config;
	session.control_fd = -1;
	result = rp_descriptor_read(descriptor_fd, &descriptor, timeout_ms(&session));
	if (result)
		goto early_failure;
	if ((config->chunk_explicit && config->chunk_size != descriptor.chunk_size)
	 || (config->depth_explicit && config->queue_depth != descriptor.queue_depth)) {
		rp_set_error("receiver tuning conflicts with sender descriptor");
		result = RP_EXIT_PROTOCOL;
		goto early_failure;
	}
	config->chunk_size = descriptor.chunk_size;
	config->queue_depth = descriptor.queue_depth;
	memcpy(session.token, descriptor.token, 16);
	session.candidate_count = enumerate_candidates(&session);
	if (!session.candidate_count) {
		result = RP_EXIT_UNAVAILABLE;
		goto early_failure;
	}
	session.channel_count = choose_channels(&descriptor, &session);
	if (!session.channel_count) {
		rp_set_error("sender and receiver channel requirements conflict");
		result = RP_EXIT_PROTOCOL;
		goto early_failure;
	}
	for (channel = 0; channel < session.channel_count; channel++) {
		result = setup_receiver_path(&session, &descriptor, channel);
		if (result)
			goto done;
	}
	rp_log(config, 0, "RDMA active: %u channel%s", session.channel_count,
		session.channel_count == 1 ? "" : "s");
	stream_called = 1;
	result = transfer_receive(&session, output_fd, consumer_pid);

done:
	if (result && session.control_fd >= 0 && !stream_called)
		send_status(&session, rp_interrupted ? RP_STATUS_CANCEL : RP_STATUS_ERROR,
			(uint32_t)(result > 255 ? RP_EXIT_IO : result), rp_last_error());
	if (!stream_called && consumer_pid >= 0) {
		close(output_fd);
		wait_consumer(consumer_pid, SIGTERM);
	}
	if (session.timer_started && !session.finished.tv_sec)
		clock_gettime(CLOCK_MONOTONIC, &session.finished);
	if (session.channel_count)
		show_summary(&session, "receive", result);
	destroy_session(&session);
	return result;

early_failure:
	if (consumer_pid >= 0) {
		close(output_fd);
		wait_consumer(consumer_pid, SIGTERM);
	}
	destroy_session(&session);
	return result;
}
