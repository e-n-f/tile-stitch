#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include <curl/curl.h>

void usage(char **argv) {
	fprintf(stderr, "Usage: %s [-o outfile] minlat minlon maxlat maxlon zoom http://whatever/{z}/{x}/{y}.png\n", argv[0]);
}

// http://wiki.openstreetmap.org/wiki/Slippy_map_tilenames
void latlon2tile(double lat, double lon, int zoom, unsigned int *x, unsigned int *y) {
	double lat_rad = lat * M_PI / 180;
	unsigned long long n = 1LL << zoom;

	*x = n * ((lon + 180) / 360);
	*y = n * (1 - (log(tan(lat_rad) + 1/cos(lat_rad)) / M_PI)) / 2;
}

int main(int argc, char **argv) {
	extern int optind;
	extern char *optarg;
	int i;

	char *outfile = NULL;
	int tilesize = 256;

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

	if (argc - optind != 6) {
		usage(argv);
		exit(EXIT_FAILURE);
	}

	double minlat = atof(argv[optind]);
	double minlon = atof(argv[optind + 1]);
	double maxlat = atof(argv[optind + 2]);
	double maxlon = atof(argv[optind + 3]);
	int zoom = atoi(argv[optind + 4]);
	char *url = argv[optind + 5];

	unsigned int x1, y1, x2, y2;
	latlon2tile(maxlat, minlon, 32, &x1, &y1);
	latlon2tile(minlat, maxlon, 32, &x2, &y2);

	unsigned int tx1 = x1 >> (32 - zoom);
	unsigned int ty1 = y1 >> (32 - zoom);
	unsigned int tx2 = x2 >> (32 - zoom);
	unsigned int ty2 = y2 >> (32 - zoom);

	printf("at zoom level %d, that's %u/%u to %u/%u\n", zoom,
		tx1, ty1, tx2, ty2);

	long long dim = (long long) (tx2 - tx1 + 1) * (ty2 - ty1 + 1) * tilesize * tilesize;
	if (dim > 10000 * 10000) {
		fprintf(stderr, "that's too big (%dx%d)\n",
			(tx2 - tx1 + 1) * tilesize,
			(ty2 - ty1 + 1) * tilesize);
		exit(EXIT_FAILURE);
	}

	unsigned char *buf = malloc(dim * 4);
	if (buf == NULL) {
		fprintf(stderr, "Can't allocate memory for %lld\n", dim * 4);
	}
	
	unsigned int tx, ty;
	for (tx = tx1; tx <= tx2; tx++) {
		for (ty = ty1; ty <= ty2; ty++) {
			int end = strlen(url) + 50;
			char url2[end];
			char *cp;
			char *out = url2;

			for (cp = url; *cp && out - url2 < end - 10; cp++) {
				if (*cp == '{' && cp[2] == '}') {
					if (cp[1] == 'z') {
						sprintf(out, "%d", zoom);
						out = out + strlen(out);
					} else if (cp[1] == 'x') {
						sprintf(out, "%u", tx);
						out = out + strlen(out);
					} else if (cp[1] == 'y') {
						sprintf(out, "%u", ty);
						out = out + strlen(out);
					} else {
						fprintf(stderr, "Unknown format token %c\n", cp[1]);
						exit(EXIT_FAILURE);
					}

					cp += 2;
				} else {
					*out++ = *cp;
				}
			}

			*out = '\0';
			printf("%s\n", url2);

			CURL *curl = curl_easy_init();
			if (curl == NULL) {
				fprintf(stderr, "Curl won't start\n");
				exit(EXIT_FAILURE);
			}

			curl_easy_setopt(curl, CURLOPT_URL, url2);
			curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

			CURLcode res = curl_easy_perform(curl);
			if (res != CURLE_OK) {
				fprintf(stderr, "Can't retrieve %s: %s\n", url2,
					curl_easy_strerror(res));
				exit(EXIT_FAILURE);
			}

			curl_easy_cleanup(curl);
		}
	}
}
