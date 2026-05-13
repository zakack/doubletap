#include "privdrop.h"
#include <unistd.h>
#include <pwd.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

int privdrop_drop(const char *username) {
	/* Check we're running as root */
	if (getuid() != 0 && geteuid() != 0) {
		fprintf(stderr, "privdrop: not running as root, cannot drop privileges\n");
		return -1;
	}

	/* Look up target user */
	struct passwd *pw = getpwnam(username);
	if (!pw) {
		fprintf(stderr, "privdrop: user '%s' not found\n", username);
		return -1;
	}

	/* Drop group first, then user */
	if (setgid(pw->pw_gid) != 0) {
		fprintf(stderr, "privdrop: setgid(%d) failed: %s\n", pw->pw_gid, strerror(errno));
		return -1;
	}

	if (setuid(pw->pw_uid) != 0) {
		fprintf(stderr, "privdrop: setuid(%d) failed: %s\n", pw->pw_uid, strerror(errno));
		return -1;
	}

	/* Verify the drop actually took effect */
	if (getuid() != pw->pw_uid || geteuid() != pw->pw_uid) {
		fprintf(stderr, "privdrop: verification failed — uid/euid mismatch\n");
		return -1;
	}

	fprintf(stderr, "privdrop: dropped privileges to user '%s' (uid %d)\n", username, pw->pw_uid);
	return 0;
}
