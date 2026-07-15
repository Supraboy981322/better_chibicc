CFLAGS = -std=c11 -g -fno-common -Wall -Wno-switch -Wextra $(shell curl-config --cflags)
LDFLAGS += $(shell curl-config --libs)

SRCS=$(wildcard *.c)
OBJS=$(SRCS:.c=.o)

TEST_SRCS=$(wildcard test/*.c)
TESTS=$(TEST_SRCS:.c=.exe)

# Stage 1

oskar: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(OBJS): oskar.h

test/%.exe: oskar test/%.c
	./oskar -Iinclude -Itest -c -o test/$*.o test/$*.c
	$(CC) -pthread -o $@ test/$*.o -xc test/common

test: $(TESTS)
	for i in $^; do echo $$i; ./$$i || exit 1; echo; done
	test/driver.sh ./oskar

test-all: test test-stage2

# Stage 2

stage2/oskar: $(OBJS:%=stage2/%)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

stage2/%.o: oskar %.c
	mkdir -p stage2/test
	./oskar -c -o $(@D)/$*.o $*.c

stage2/test/%.exe: stage2/oskar test/%.c
	mkdir -p stage2/test
	./stage2/oskar -Iinclude -Itest -c -o stage2/test/$*.o test/$*.c
	$(CC) -pthread -o $@ stage2/test/$*.o -xc test/common

test-stage2: $(TESTS:test/%=stage2/test/%)
	for i in $^; do echo $$i; ./$$i || exit 1; echo; done
	test/driver.sh ./stage2/oskar

# Misc.

clean:
	rm -rf oskar tmp* $(TESTS) test/*.s test/*.exe stage2
	find * -type f '(' -name '*~' -o -name '*.o' ')' -exec rm {} ';'

.PHONY: test clean test-stage2
