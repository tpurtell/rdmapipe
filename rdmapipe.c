#define _GNU_SOURCE

#include "rdmapipe.h"
#include "protocol.h"
#include "transport.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

enum command_mode {
	MODE_SHORTHAND,
	MODE_SEND,
	MODE_RECEIVE
};

struct string_list {
	char **items;
	size_t count;
	size_t capacity;
};

struct cli_options {
	struct rp_config config;
	enum command_mode mode;
	int mode_set;
	int remote_discard;
	int remote_path_explicit;
	int ssh_explicit;
	int port_explicit;
	char remote_device[RP_MAX_DEVICE_FILTER + 1];
	const char *remote_path;
	const char *ssh_program;
	const char *encoded_argv;
	struct string_list ssh_options;
};

enum long_option_id {
	OPT_SEND = 256,
	OPT_RECEIVE,
	OPT_CHANNELS,
	OPT_CHUNK_SIZE,
	OPT_QUEUE_DEPTH,
	OPT_DEVICE,
	OPT_REMOTE_DEVICE,
	OPT_PORT,
	OPT_TIMEOUT,
	OPT_DISCARD,
	OPT_REMOTE_DISCARD,
	OPT_REMOTE_PATH,
	OPT_SSH,
	OPT_SSH_OPTION,
	OPT_EXEC_ARGV,
	OPT_VERSION
};

static const struct option long_options[] = {
	{ "send", no_argument, NULL, OPT_SEND },
	{ "recv", no_argument, NULL, OPT_RECEIVE },
	{ "channels", required_argument, NULL, OPT_CHANNELS },
	{ "rails", required_argument, NULL, OPT_CHANNELS },
	{ "chunk-size", required_argument, NULL, OPT_CHUNK_SIZE },
	{ "queue-depth", required_argument, NULL, OPT_QUEUE_DEPTH },
	{ "device", required_argument, NULL, OPT_DEVICE },
	{ "remote-device", required_argument, NULL, OPT_REMOTE_DEVICE },
	{ "port", required_argument, NULL, OPT_PORT },
	{ "timeout", required_argument, NULL, OPT_TIMEOUT },
	{ "discard", no_argument, NULL, OPT_DISCARD },
	{ "remote-discard", no_argument, NULL, OPT_REMOTE_DISCARD },
	{ "remote-path", required_argument, NULL, OPT_REMOTE_PATH },
	{ "ssh", required_argument, NULL, OPT_SSH },
	{ "ssh-option", required_argument, NULL, OPT_SSH_OPTION },
	{ "exec-argv", required_argument, NULL, OPT_EXEC_ARGV },
	{ "quiet", no_argument, NULL, 'q' },
	{ "verbose", no_argument, NULL, 'v' },
	{ "help", no_argument, NULL, 'h' },
	{ "version", no_argument, NULL, OPT_VERSION },
	{ NULL, 0, NULL, 0 }
};

static void usage(FILE *output)
{
	fputs(
		"Usage:\n"
		"  rdmapipe [OPTIONS] --send\n"
		"  rdmapipe [OPTIONS] --recv\n"
		"  rdmapipe [OPTIONS] HOST -- COMMAND [ARG ...]\n"
		"\n"
		"Transport:\n"
		"  --channels=auto|1|2       channel count (default: auto)\n"
		"  --rails=auto|1|2          alias for --channels\n"
		"  --chunk-size=SIZE         registered payload slot (default: 2M)\n"
		"  --queue-depth=N           slots per channel (default: 4)\n"
		"  --device=LIST             local verbs/netdevice/address filter\n"
		"  --port=PORT               sender bootstrap base port (default: 0)\n"
		"  --timeout=SECONDS         setup/stall timeout (default: 300)\n"
		"  --discard                 receive and validate without stdout writes\n"
		"\n"
		"SSH shorthand:\n"
		"  --remote-device=LIST      receiver device filter\n"
		"  --remote-discard          discard at the receiver\n"
		"  --remote-path=PROGRAM     remote binary (default: rdmapipe)\n"
		"  --ssh=PROGRAM             SSH executable (default: ssh)\n"
		"  --ssh-option=ARG          repeatable argument before HOST\n"
		"\n"
		"Diagnostics:\n"
		"  -q, --quiet               suppress topology and statistics\n"
		"  -v, --verbose             add diagnostics; repeat for detail\n"
		"  -h, --help                show this help\n"
		"      --version             show build/protocol defaults\n",
		output);
}

static void version(void)
{
	printf("rdmapipe %s\nprotocol %d\nlibibverbs enabled\ndefaults: chunk=%u depth=%u channels=auto timeout=%u\n",
		RP_VERSION, RP_PROTOCOL_VERSION, RP_DEFAULT_CHUNK, RP_DEFAULT_DEPTH,
		RP_DEFAULT_TIMEOUT);
}

static void signal_handler(int number)
{
	rp_interrupted = number;
}

static void install_signal_handlers(void)
{
	struct sigaction action;
	memset(&action, 0, sizeof action);
	action.sa_handler = signal_handler;
	sigemptyset(&action.sa_mask);
	sigaction(SIGINT, &action, NULL);
	sigaction(SIGTERM, &action, NULL);
	signal(SIGPIPE, SIG_IGN);
}

static void reset_child_signals(void)
{
	signal(SIGINT, SIG_DFL);
	signal(SIGTERM, SIG_DFL);
	signal(SIGPIPE, SIG_DFL);
}

static int list_append(struct string_list *list, char *item)
{
	if (list->count == list->capacity) {
		size_t capacity = list->capacity ? list->capacity * 2 : 4;
		char **items = realloc(list->items, capacity * sizeof *items);
		if (!items)
			return -1;
		list->items = items;
		list->capacity = capacity;
	}
	list->items[list->count++] = item;
	return 0;
}

static int parse_u32_option(const char *name, const char *text,
			    uint32_t minimum, uint32_t maximum, uint32_t *value)
{
	char *end;
	unsigned long long number;

	if (!*text || *text == '-')
		goto invalid;
	errno = 0;
	number = strtoull(text, &end, 10);
	if (errno || *end || number < minimum || number > maximum)
		goto invalid;
	*value = (uint32_t)number;
	return 0;

invalid:
	rp_error("invalid %s: %s", name, text);
	return -1;
}

static int parse_size(const char *text, uint32_t *value)
{
	char *end;
	unsigned long long number, multiplier = 1;

	if (!*text || *text == '-')
		goto invalid;
	errno = 0;
	number = strtoull(text, &end, 10);
	if (errno)
		goto invalid;
	if (*end) {
		if (end[1])
			goto invalid;
		switch (toupper((unsigned char)*end)) {
		case 'K': multiplier = 1024ULL; break;
		case 'M': multiplier = 1024ULL * 1024ULL; break;
		case 'G': multiplier = 1024ULL * 1024ULL * 1024ULL; break;
		default: goto invalid;
		}
	}
	if (number > UINT32_MAX / multiplier)
		goto invalid;
	number *= multiplier;
	if (number < 4096 || number > 8U * 1024U * 1024U || (number & 63U))
		goto invalid;
	*value = (uint32_t)number;
	return 0;

invalid:
	rp_error("invalid chunk size: %s (expected 4K..8M, multiple of 64)", text);
	return -1;
}

static int safe_remote_word(const char *word)
{
	const unsigned char *position = (const unsigned char *)word;
	if (!*position || *position == '-')
		return 0;
	for (; *position; position++) {
		if (!isalnum(*position) && !strchr("_./~+-", *position))
			return 0;
	}
	return 1;
}

static int safe_device_filter(const char *filter)
{
	const unsigned char *position = (const unsigned char *)filter;
	int component = 0;

	if (!*position || strlen(filter) > RP_MAX_DEVICE_FILTER)
		return 0;
	for (; *position; position++) {
		if (*position == ',') {
			if (!component)
				return 0;
			component = 0;
		} else if (isalnum(*position) || strchr("_.:-", *position)) {
			component = 1;
		} else {
			return 0;
		}
	}
	return component;
}

static int set_mode(struct cli_options *options, enum command_mode mode)
{
	if (options->mode_set && options->mode != mode) {
		rp_error("--send and --recv are mutually exclusive");
		return -1;
	}
	options->mode = mode;
	options->mode_set = 1;
	return 0;
}

static int parse_channels(const char *text, uint32_t *channels)
{
	if (!strcmp(text, "auto"))
		*channels = 0;
	else if (!strcmp(text, "1"))
		*channels = 1;
	else if (!strcmp(text, "2"))
		*channels = 2;
	else {
		rp_error("invalid channel count: %s (expected auto, 1, or 2)", text);
		return -1;
	}
	return 0;
}

static int parse_options(int argc, char **argv, struct cli_options *options)
{
	int option;

	memset(options, 0, sizeof *options);
	rp_config_init(&options->config);
	options->remote_path = getenv("RDMAPIPE_REMOTE_PATH");
	options->ssh_program = getenv("RDMAPIPE_SSH");
	if (!options->remote_path || !*options->remote_path)
		options->remote_path = "rdmapipe";
	if (!options->ssh_program || !*options->ssh_program)
		options->ssh_program = "ssh";
	opterr = 0;
	while ((option = getopt_long(argc, argv, "+qvh", long_options, NULL)) != -1) {
		switch (option) {
		case OPT_SEND:
			if (set_mode(options, MODE_SEND) < 0)
				return -1;
			break;
		case OPT_RECEIVE:
			if (set_mode(options, MODE_RECEIVE) < 0)
				return -1;
			break;
		case OPT_CHANNELS:
			if (parse_channels(optarg, &options->config.channels) < 0)
				return -1;
			options->config.channels_explicit = 1;
			break;
		case OPT_CHUNK_SIZE:
			if (parse_size(optarg, &options->config.chunk_size) < 0)
				return -1;
			options->config.chunk_explicit = 1;
			break;
		case OPT_QUEUE_DEPTH:
			if (parse_u32_option("queue depth", optarg, 2, 4096,
					     &options->config.queue_depth) < 0)
				return -1;
			options->config.depth_explicit = 1;
			break;
		case OPT_DEVICE:
			if (!safe_device_filter(optarg)) {
				rp_error("invalid device filter: %s", optarg);
				return -1;
			}
			snprintf(options->config.device_filter,
				sizeof options->config.device_filter, "%s", optarg);
			break;
		case OPT_REMOTE_DEVICE:
			if (!safe_device_filter(optarg)) {
				rp_error("invalid remote device filter: %s", optarg);
				return -1;
			}
			snprintf(options->remote_device, sizeof options->remote_device, "%s", optarg);
			break;
		case OPT_PORT:
			if (parse_u32_option("bootstrap port", optarg, 0, 65535,
					     &options->config.port) < 0)
				return -1;
			options->port_explicit = 1;
			break;
		case OPT_TIMEOUT:
			if (parse_u32_option("timeout", optarg, 1, 86400,
					     &options->config.timeout) < 0)
				return -1;
			break;
		case OPT_DISCARD:
			options->config.discard = 1;
			break;
		case OPT_REMOTE_DISCARD:
			options->remote_discard = 1;
			break;
		case OPT_REMOTE_PATH:
			options->remote_path = optarg;
			options->remote_path_explicit = 1;
			break;
		case OPT_SSH:
			options->ssh_program = optarg;
			options->ssh_explicit = 1;
			break;
		case OPT_SSH_OPTION:
			if (list_append(&options->ssh_options, optarg) < 0) {
				rp_error("out of memory");
				return -1;
			}
			break;
		case OPT_EXEC_ARGV:
			options->encoded_argv = optarg;
			break;
		case 'q': options->config.quiet = 1; break;
		case 'v': options->config.verbose++; break;
		case 'h': usage(stdout); exit(0);
		case OPT_VERSION: version(); exit(0);
		default:
			rp_error("unknown or incomplete option: %s",
				optind > 0 ? argv[optind - 1] : "option");
			return -1;
		}
	}
	return 0;
}

static int run_receiver(struct cli_options *options)
{
	char **consumer_argv = NULL;
	int pipe_fds[2] = { -1, -1 };
	pid_t consumer = -1;
	int output_fd = STDOUT_FILENO;
	int result;

	if (options->encoded_argv) {
		consumer_argv = rp_argv_decode(options->encoded_argv);
		if (!consumer_argv)
			return RP_EXIT_USAGE;
		if (pipe2(pipe_fds, O_CLOEXEC) < 0) {
			rp_error("cannot create consumer pipe: %s", strerror(errno));
			rp_argv_free(consumer_argv);
			return RP_EXIT_IO;
		}
		consumer = fork();
		if (consumer < 0) {
			rp_error("cannot fork remote consumer: %s", strerror(errno));
			close(pipe_fds[0]);
			close(pipe_fds[1]);
			rp_argv_free(consumer_argv);
			return RP_EXIT_IO;
		}
		if (!consumer) {
			if (setpgid(0, 0) < 0) {
				rp_error("cannot create remote consumer process group: %s",
					strerror(errno));
				_exit(126);
			}
			reset_child_signals();
			close(pipe_fds[1]);
			if (dup2(pipe_fds[0], STDIN_FILENO) < 0) {
				rp_error("cannot attach remote consumer stdin: %s", strerror(errno));
				_exit(126);
			}
			close(pipe_fds[0]);
			execvp(consumer_argv[0], consumer_argv);
			rp_error("cannot execute %s: %s", consumer_argv[0], strerror(errno));
			_exit(errno == ENOENT ? 127 : 126);
		}
		if (setpgid(consumer, consumer) < 0 && errno != EACCES && errno != ESRCH)
			rp_log(&options->config, 1,
				"cannot confirm remote consumer process group: %s", strerror(errno));
		close(pipe_fds[0]);
		output_fd = pipe_fds[1];
		rp_argv_free(consumer_argv);
	}
	result = rp_receive_stream(&options->config, STDIN_FILENO, output_fd, consumer);
	if (result)
		rp_error("receive failed: %s", rp_last_error());
	return result;
}

static int child_status(int status)
{
	if (WIFEXITED(status))
		return WEXITSTATUS(status);
	if (WIFSIGNALED(status))
		return 128 + WTERMSIG(status);
	return RP_EXIT_IO;
}

static int run_shorthand(struct cli_options *options, const char *host,
			 char *const command_argv[])
{
	char *encoded = NULL, *encoded_option = NULL, *device_option = NULL;
	char timeout_option[64];
	char **ssh_argv = NULL;
	int descriptor_pipe[2] = { -1, -1 };
	pid_t sender = -1, ssh = -1;
	int sender_result = RP_EXIT_IO, ssh_result = RP_EXIT_IO;
	size_t argc = 0, i;

	if (!*host || *host == '-') {
		rp_error("invalid SSH host: %s", host);
		return RP_EXIT_USAGE;
	}
	if (!safe_remote_word(options->remote_path)) {
		rp_error("unsafe remote executable path: %s", options->remote_path);
		return RP_EXIT_USAGE;
	}
	encoded = rp_argv_encode(command_argv);
	if (!encoded) {
		rp_error("cannot encode remote command: %s", rp_last_error());
		return RP_EXIT_USAGE;
	}
	if (asprintf(&encoded_option, "--exec-argv=%s", encoded) < 0)
		goto memory_error;
	if (*options->remote_device
	 && asprintf(&device_option, "--device=%s", options->remote_device) < 0)
		goto memory_error;
	snprintf(timeout_option, sizeof timeout_option, "--timeout=%u", options->config.timeout);
	argc = 1 + options->ssh_options.count + 1 + 2 + 1 + 1;
	if (device_option)
		argc++;
	if (options->remote_discard)
		argc++;
	if (options->config.quiet)
		argc++;
	argc += (size_t)options->config.verbose;
	ssh_argv = calloc(argc + 1, sizeof *ssh_argv);
	if (!ssh_argv)
		goto memory_error;
	argc = 0;
	ssh_argv[argc++] = (char *)options->ssh_program;
	for (i = 0; i < options->ssh_options.count; i++)
		ssh_argv[argc++] = options->ssh_options.items[i];
	ssh_argv[argc++] = (char *)host;
	ssh_argv[argc++] = (char *)options->remote_path;
	ssh_argv[argc++] = "--recv";
	ssh_argv[argc++] = timeout_option;
	if (device_option)
		ssh_argv[argc++] = device_option;
	if (options->remote_discard)
		ssh_argv[argc++] = "--discard";
	if (options->config.quiet)
		ssh_argv[argc++] = "--quiet";
	for (i = 0; i < (size_t)options->config.verbose; i++)
		ssh_argv[argc++] = "--verbose";
	ssh_argv[argc++] = encoded_option;
	ssh_argv[argc] = NULL;
	if (pipe2(descriptor_pipe, O_CLOEXEC) < 0) {
		rp_error("cannot create descriptor pipe: %s", strerror(errno));
		goto failed;
	}
	sender = fork();
	if (sender < 0) {
		rp_error("cannot fork local sender: %s", strerror(errno));
		goto failed;
	}
	if (!sender) {
		int code;
		close(descriptor_pipe[0]);
		code = rp_send_stream(&options->config, STDIN_FILENO, descriptor_pipe[1]);
		if (code)
			rp_error("send failed: %s", rp_last_error());
		close(descriptor_pipe[1]);
		_exit(code > 255 ? RP_EXIT_IO : code);
	}
	ssh = fork();
	if (ssh < 0) {
		rp_error("cannot fork SSH: %s", strerror(errno));
		kill(sender, SIGTERM);
		goto failed;
	}
	if (!ssh) {
		reset_child_signals();
		close(descriptor_pipe[1]);
		if (dup2(descriptor_pipe[0], STDIN_FILENO) < 0)
			_exit(127);
		close(descriptor_pipe[0]);
		execvp(ssh_argv[0], ssh_argv);
		rp_error("cannot execute SSH program %s: %s", ssh_argv[0], strerror(errno));
		_exit(127);
	}
	close(descriptor_pipe[0]);
	close(descriptor_pipe[1]);
	descriptor_pipe[0] = descriptor_pipe[1] = -1;
	while (sender > 0 || ssh > 0) {
		int status;
		pid_t child = waitpid(-1, &status, 0);
		if (child < 0 && errno == EINTR) {
			if (rp_interrupted) {
				if (sender > 0)
					kill(sender, (int)rp_interrupted);
				if (ssh > 0)
					kill(ssh, (int)rp_interrupted);
			}
			continue;
		}
		if (child < 0) {
			rp_error("cannot supervise sender/SSH: %s", strerror(errno));
			break;
		}
		if (child == sender) {
			sender_result = child_status(status);
			sender = -1;
		} else if (child == ssh) {
			ssh_result = child_status(status);
			ssh = -1;
			if (ssh_result && sender > 0)
				kill(sender, SIGTERM);
		}
	}
	if (sender_result && ssh_result && sender_result != ssh_result)
		rp_error("local sender exited %d; SSH/receiver exited %d",
			sender_result, ssh_result);
	free(ssh_argv);
	free(device_option);
	free(encoded_option);
	free(encoded);
	if (rp_interrupted)
		return rp_interrupted_exit_code();
	return ssh_result ? ssh_result : sender_result;

memory_error:
	rp_error("out of memory while building SSH command");
failed:
	if (descriptor_pipe[0] >= 0)
		close(descriptor_pipe[0]);
	if (descriptor_pipe[1] >= 0)
		close(descriptor_pipe[1]);
	if (sender > 0)
		waitpid(sender, NULL, 0);
	free(ssh_argv);
	free(device_option);
	free(encoded_option);
	free(encoded);
	return RP_EXIT_IO;
}

int main(int argc, char **argv)
{
	struct cli_options options;
	int positional, result;

	install_signal_handlers();
	if (parse_options(argc, argv, &options) < 0) {
		usage(stderr);
		result = RP_EXIT_USAGE;
		goto done;
	}
	positional = argc - optind;
	if (options.mode_set) {
		if (positional || options.remote_discard || *options.remote_device
		 || options.remote_path_explicit || options.ssh_explicit
		 || options.ssh_options.count) {
			rp_error("raw --send/--recv mode does not accept shorthand arguments");
			result = RP_EXIT_USAGE;
			goto done;
		}
		if (options.mode == MODE_SEND && (options.config.discard || options.encoded_argv)) {
			rp_error("--discard and --exec-argv are receiver-only");
			result = RP_EXIT_USAGE;
			goto done;
		}
		if (options.mode == MODE_RECEIVE && options.port_explicit) {
			rp_error("--port is sender-only");
			result = RP_EXIT_USAGE;
			goto done;
		}
		if (options.mode == MODE_RECEIVE)
			result = run_receiver(&options);
		else {
			result = rp_send_stream(&options.config, STDIN_FILENO, STDOUT_FILENO);
			if (result)
				rp_error("send failed: %s", rp_last_error());
		}
	} else {
		if (positional < 3 || strcmp(argv[optind + 1], "--")
		 || options.encoded_argv || options.config.discard) {
			rp_error("shorthand requires HOST -- COMMAND [ARG ...]");
			usage(stderr);
			result = RP_EXIT_USAGE;
			goto done;
		}
		result = run_shorthand(&options, argv[optind], &argv[optind + 2]);
	}

done:
	free(options.ssh_options.items);
	return result;
}
