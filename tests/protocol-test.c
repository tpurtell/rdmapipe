#define _POSIX_C_SOURCE 200809L

#include "protocol.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failures;

#define CHECK(condition) do { \
	if (!(condition)) { \
		fprintf(stderr, "protocol-test:%d: check failed: %s\n", __LINE__, #condition); \
		failures++; \
	} \
} while (0)

static void descriptor_tests(void)
{
	struct rp_descriptor source, parsed;
	char text[RP_MAX_DESCRIPTOR + 1];
	int length, pipe_fds[2];
	unsigned i;

	memset(&source, 0, sizeof source);
	source.version = RP_PROTOCOL_VERSION;
	for (i = 0; i < 16; i++)
		source.token[i] = (unsigned char)i;
	source.chunk_size = RP_DEFAULT_CHUNK;
	source.queue_depth = RP_DEFAULT_DEPTH;
	source.channels = 0;
	source.endpoint_count = 2;
	snprintf(source.endpoints[0].address, sizeof source.endpoints[0].address,
		"10.55.0.12");
	source.endpoints[0].port = 45123;
	source.endpoints[0].rate_gbps = 400;
	snprintf(source.endpoints[1].address, sizeof source.endpoints[1].address,
		"10.55.0.13");
	source.endpoints[1].port = 45124;
	source.endpoints[1].rate_gbps = 200;
	length = rp_descriptor_format(&source, text, sizeof text);
	CHECK(length > 0);
	CHECK((size_t)length == strlen(text));
	CHECK(text[length - 1] == '\n');
	text[length - 1] = '\0';
	CHECK(rp_descriptor_parse(text, &parsed) == 0);
	CHECK(parsed.version == source.version);
	CHECK(!memcmp(parsed.token, source.token, sizeof source.token));
	CHECK(parsed.chunk_size == source.chunk_size);
	CHECK(parsed.queue_depth == source.queue_depth);
	CHECK(parsed.channels == source.channels);
	CHECK(parsed.endpoint_count == source.endpoint_count);
	CHECK(!strcmp(parsed.endpoints[1].address, "10.55.0.13"));
	CHECK(parsed.endpoints[1].port == 45124);

	CHECK(rp_descriptor_parse(
		"{\"unknown\":[true,{\"x\":null}],\"endpoints\":[{\"rate\":200,\"port\":7,\"address\":\"10.55.0.1\"}],\"channels\":1,\"depth\":2,\"chunk\":4096,\"token\":\"000102030405060708090a0b0c0d0e0f\",\"rdmapipe\":1}",
		&parsed) == 0);
	CHECK(parsed.channels == 1 && parsed.endpoint_count == 1);
	CHECK(rp_descriptor_parse("{}", &parsed) == RP_EXIT_USAGE);
	CHECK(rp_descriptor_parse(
		"{\"rdmapipe\":1,\"token\":\"000102030405060708090a0b0c0d0e0f\",\"chunk\":4097,\"depth\":8,\"channels\":0,\"endpoints\":[{\"address\":\"10.55.0.1\",\"port\":1,\"rate\":200}]}",
		&parsed) == RP_EXIT_USAGE);

	CHECK(pipe(pipe_fds) == 0);
	CHECK(rp_write_all(pipe_fds[1], text, strlen(text)) == 0);
	CHECK(rp_write_all(pipe_fds[1], "\nstill-open", 11) == 0);
	CHECK(rp_descriptor_read(pipe_fds[0], &parsed, 1000) == 0);
	CHECK(parsed.endpoint_count == 2);
	close(pipe_fds[0]);
	close(pipe_fds[1]);

	CHECK(rp_descriptor_parse(
		"{\"rdmapipe\":1,\"token\":\"000102030405060708090a0b0c0d0e0f\",\"chunk\":4096,\"depth\":2,\"channels\":1,\"future\":\"a long optional value with \\\"quotes\\\", \\\\slashes and \\u263a escapes\",\"endpoints\":[{\"address\":\"10.55.0.1\",\"port\":7,\"rate\":200}]} ",
		&parsed) == 0);

	CHECK(pipe(pipe_fds) == 0);
	CHECK(rp_descriptor_read(pipe_fds[0], &parsed, 20) == RP_EXIT_TIMEOUT);
	close(pipe_fds[0]);
	close(pipe_fds[1]);
}

static void argv_tests(void)
{
	char *input[] = {
		"sh", "-c", "printf '%s' \"$1\"", "", "a b", "quote'\"", "line\nfeed", NULL
	};
	char *empty[] = { NULL };
	char *encoded;
	char **decoded;
	size_t i;

	encoded = rp_argv_encode(input);
	CHECK(encoded != NULL);
	decoded = rp_argv_decode(encoded);
	CHECK(decoded != NULL);
	if (decoded) {
		for (i = 0; input[i]; i++)
			CHECK(decoded[i] && !strcmp(decoded[i], input[i]));
		CHECK(decoded[i] == NULL);
	}
	rp_argv_free(decoded);
	free(encoded);
	CHECK(rp_argv_encode(empty) == NULL);
	CHECK(rp_argv_decode("not base64") == NULL);
	CHECK(rp_argv_decode("YQ==") == NULL);
}

int main(void)
{
	descriptor_tests();
	argv_tests();
	if (failures) {
		fprintf(stderr, "%d protocol test(s) failed\n", failures);
		return 1;
	}
	puts("protocol tests passed");
	return 0;
}
