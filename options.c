#include "options.h"
#include <stdio.h>
#include <string.h>

int handleOptions(int argc, char** argv)
{
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
			printf("Chim - Controlled vim\n\n");
			printf("Usage:\n");
			printf("  chim [options] [file]\n\n");
			printf("Options:\n");
			printf("  --help, -h Show help message\n");
			printf("  --version Show version\n");
			return 1;
		}

		if (strcmp(argv[i], "--version") == 0) {
			printf("Chim v0.1.1\n");
			return 1;
		}
	}

	return 0;
}
