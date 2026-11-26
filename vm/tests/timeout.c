/*
 * timeout SECONDS COMMAND [ARG ...]: run COMMAND, killing it (SIGKILL)
 * if it runs longer than SECONDS; exit with its status (128 plus the
 * signal if a signal ended it), or 142 (as for SIGALRM) if it was
 * killed. A signal to the command itself is not enough: Go's runtime
 * does not die of SIGALRM. Built by the tests that use it.
 */
#define _POSIX_C_SOURCE 200809L
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

static volatile sig_atomic_t expired;

static void alarmed(int sig) {
	(void)sig;
	expired = 1;
}

int main(int argc, char **argv) {
	if (argc < 3) {
		fprintf(stderr, "usage: timeout SECONDS COMMAND [ARG ...]\n");
		return 2;
	}
	unsigned secs = (unsigned)strtoul(argv[1], NULL, 10);
	pid_t pid = fork();
	if (pid < 0) {
		perror("fork");
		return 2;
	}
	if (pid == 0) {
		execvp(argv[2], argv + 2);
		fprintf(stderr, "exec %s: ", argv[2]);
		perror(NULL);
		_exit(127);
	}
	struct sigaction sa = {0};
	sa.sa_handler = alarmed; /* no SA_RESTART: waitpid is interrupted */
	sigaction(SIGALRM, &sa, NULL);
	alarm(secs);
	int st;
	while (waitpid(pid, &st, 0) < 0) {
		if (expired) {
			kill(pid, SIGKILL);
			waitpid(pid, &st, 0);
			return 142;
		}
	}
	alarm(0);
	return WIFSIGNALED(st) ? 128 + WTERMSIG(st) : WEXITSTATUS(st);
}
