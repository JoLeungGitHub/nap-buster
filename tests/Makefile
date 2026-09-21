CC ?= cc
CFLAGS ?= -std=c11 -Wall -Wextra -Werror -pedantic

.PHONY: all test clean

all: test_nap_detector test_alert_policy

test_nap_detector: test_nap_detector.c ../worker_src/c/nap_detector.c ../worker_src/c/nap_detector.h
	$(CC) $(CFLAGS) -I../worker_src/c test_nap_detector.c ../worker_src/c/nap_detector.c -o $@

test_alert_policy: test_alert_policy.c ../src/common/alert_policy.h ../worker_src/c/sleep_fallback.h ../worker_src/c/nap_detector.c ../worker_src/c/nap_detector.h
	$(CC) $(CFLAGS) -I../worker_src/c test_alert_policy.c ../worker_src/c/nap_detector.c -o $@

test: all
	./test_nap_detector
	./test_alert_policy

clean:
	$(RM) test_nap_detector test_alert_policy
