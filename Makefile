PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin
MANDIR ?= $(PREFIX)/share/man
CC ?= cc
CFLAGS ?= -O2 -g
CPPFLAGS ?=
WARNINGS = -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wstrict-prototypes
LDLIBS = -libverbs

PROGRAM = rdmapipe
OBJECTS = rdmapipe.o protocol.o transport.o util.o
TEST_PROGRAM = tests/protocol-test

.PHONY: all clean check install uninstall

all: $(PROGRAM)

$(PROGRAM): $(OBJECTS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJECTS) $(LDLIBS)

%.o: %.c rdmapipe.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) -std=c11 -c -o $@ $<

protocol.o: protocol.h
transport.o: transport.h protocol.h
rdmapipe.o: protocol.h transport.h

$(TEST_PROGRAM): tests/protocol-test.c protocol.o util.o protocol.h rdmapipe.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) -std=c11 -I. -o $@ \
		tests/protocol-test.c protocol.o util.o

check: all $(TEST_PROGRAM)
	./tests/test.sh

install: all
	install -d "$(DESTDIR)$(BINDIR)"
	install -m 755 $(PROGRAM) "$(DESTDIR)$(BINDIR)/$(PROGRAM)"
	install -d "$(DESTDIR)$(MANDIR)/man1"
	install -m 644 rdmapipe.1 "$(DESTDIR)$(MANDIR)/man1/rdmapipe.1"

uninstall:
	rm -f "$(DESTDIR)$(BINDIR)/$(PROGRAM)"
	rm -f "$(DESTDIR)$(MANDIR)/man1/rdmapipe.1"

clean:
	rm -f $(PROGRAM) $(OBJECTS) $(TEST_PROGRAM)
