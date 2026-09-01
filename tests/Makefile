CC ?= cc
CFLAGS ?= -std=c11 -Wall -Wextra -Werror -pedantic

.PHONY: all test clean

all: test_nap_detector

test_nap_detector: test_nap_detector.c ../worker_src/c/nap_detector.c ../worker_src/c/nap_detector.h
	$(CC) $(CFLAGS) -I../worker_src/c test_nap_detector.c ../worker_src/c/nap_detector.c -o $@

test: test_nap_detector
	./test_nap_detector

clean:
	$(RM) test_nap_detector
