#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <curl/curl.h>

void usage(char **argv) {
	fprintf(stderr, "Usage: %s [-o outfile] minlat minlon maxlat maxlon zoom http://whatever/{z}/{x}/{y}.png\n", argv[0]);
}

int main(int argc, char **argv) {
	extern int optind;
	extern char *optarg;
	int i;

	char *outfile = NULL;

	while ((i = getopt(argc, argv, "o:")) != -1) {
		switch (i) {
		case 'o':
			outfile = optarg;
			break;

		default:
			usage(argv);
			exit(EXIT_FAILURE);
		}
	}
}
