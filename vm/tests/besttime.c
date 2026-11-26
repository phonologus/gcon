/*
 * besttime COMMAND [ARG ...]: run COMMAND three times and print the
 * best wall time, in nanoseconds; exit 1 if a run fails. For bench.sh,
 * which builds it.
 */
#define _POSIX_C_SOURCE 200809L
#include <stdint.h>
#include <stdio.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int64_t now(void) {
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (int64_t)t.tv_sec * 1000000000 + t.tv_nsec;
}

int main(int argc, char **argv) {
	if (argc < 2) {
		fprintf(stderr, "usage: besttime COMMAND [ARG ...]\n");
		return 2;
	}
	int64_t best = -1;
	for (int i = 0; i < 3; i++) {
		int64_t t = now();
		pid_t pid = fork();
		if (pid == 0) {
			execvp(argv[1], argv + 1);
			_exit(127);
		}
		int st;
		if (pid < 0 || waitpid(pid, &st, 0) < 0 || !WIFEXITED(st) || WEXITSTATUS(st)) {
			fprintf(stderr, "besttime: failed: %s\n", argv[1]);
			return 1;
		}
		t = now() - t;
		if (best < 0 || t < best)
			best = t;
	}
	printf("%lld\n", (long long)best);
	return 0;
}
