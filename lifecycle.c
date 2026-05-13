#include "lifecycle.h"
#include <signal.h>
#include <unistd.h>
#include <stdio.h>

static volatile sig_atomic_t g_running = 1;

static void on_signal(int sig) {
	(void)sig;
	// async-signal-safe: use write(), not fprintf
	static const char msg[] = "osu-interceptd: shutting down...\n";
	write(STDERR_FILENO, msg, sizeof(msg) - 1);
	g_running = 0;
}

void lifecycle_init(void) {
	struct sigaction sa = { .sa_handler = on_signal };
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = 0;
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
}

int lifecycle_is_running(void) {
	return g_running;
}

void lifecycle_shutdown(void) {
	fprintf(stderr, "osu-interceptd: shutdown complete\n");
}
