/*
 * gcon-vm: the virtual machine for gcon's bytecode (docs/design.md).
 * Nothing runs yet; this is the entry point the build produces.
 */
#include <stdio.h>
#include <string.h>

#define GCONVM_VERSION "0.0.0"

int main(int argc, char **argv) {
	if (argc == 2 && strcmp(argv[1], "-version") == 0) {
		printf("gcon-vm %s\n", GCONVM_VERSION);
		return 0;
	}
	fprintf(stderr, "usage: gcon-vm -version\n");
	return 2;
}
