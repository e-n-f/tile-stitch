#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include <curl/curl.h>
#include <jpeglib.h>
#include <png.h>

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

struct data {
	char *buf;
	int len;
	int nalloc;
};

struct image {
	unsigned char *buf;
	int depth;
	int width;
	int height;
};

size_t curl_receive(char *ptr, size_t size, size_t nmemb, void *v) {
	struct data *data = v;

	if (data->len + size * nmemb >= data->nalloc) {
		data->nalloc += size * nmemb + 50000;
		data->buf = realloc(data->buf, data->nalloc);
	}

	memcpy(data->buf + data->len, ptr, size * nmemb);
	data->len += size * nmemb;

	return size * nmemb;
};

struct image *read_jpeg(char *s, int len) {
	struct jpeg_decompress_struct cinfo;
	struct jpeg_error_mgr jerr;

	cinfo.err = jpeg_std_error(&jerr);
	jpeg_create_decompress(&cinfo);
	jpeg_mem_src(&cinfo, (unsigned char *) s, len);
	jpeg_read_header(&cinfo, TRUE);
	jpeg_start_decompress(&cinfo);

	int row_stride = cinfo.output_width * cinfo.output_components;
	JSAMPARRAY buffer = (*cinfo.mem->alloc_sarray) ((j_common_ptr) &cinfo, JPOOL_IMAGE, row_stride, 1);

	struct image *i = malloc(sizeof(struct image));
	i->buf = malloc(cinfo.output_width * cinfo.output_height * cinfo.output_components);
	i->width = cinfo.output_width;
	i->height = cinfo.output_height;
	i->depth = cinfo.output_components;

	unsigned char *here = i->buf;
	while (cinfo.output_scanline < cinfo.output_height) {
		jpeg_read_scanlines(&cinfo, buffer, 1);
		memcpy(here, buffer[0], row_stride);
		here += row_stride;
	}

	jpeg_finish_decompress(&cinfo);
	jpeg_destroy_decompress(&cinfo);

	return i;
}

static void fail(png_structp png_ptr, png_const_charp error_msg) {
	fprintf(stderr, "PNG error %s\n", error_msg);
	exit(EXIT_FAILURE);
}

void png_read_data(png_structp png_ptr, png_bytep out, png_size_t toread) {
	struct data *d = (struct data *) png_get_io_ptr(png_ptr);
	memcpy(out, d->buf + d->len, toread);
	d->len+= toread;
}

struct image *read_png(char *s, int len) {
	png_structp png_ptr;
	png_infop info_ptr;

	png_ptr = png_create_read_struct(PNG_LIBPNG_VER_STRING, fail, fail, fail);
	if (png_ptr == NULL) {
		fprintf(stderr, "PNG failure (write struct)\n");
		exit(EXIT_FAILURE);
	}
	info_ptr = png_create_info_struct(png_ptr);
	if (info_ptr == NULL) {
		png_destroy_write_struct(&png_ptr, NULL);
		fprintf(stderr, "PNG failure (info struct)\n");
		exit(EXIT_FAILURE);
	}

	struct data d;
	d.buf = s;
	d.len = 0;
	d.nalloc = len;

	png_set_read_fn(png_ptr, &d, png_read_data);
	png_read_info(png_ptr, info_ptr);

	struct image *i = malloc(sizeof(struct image));
	i->width = png_get_image_width(png_ptr, info_ptr);
	i->height = png_get_image_height(png_ptr, info_ptr);

	printf("%dx%d\n", i->width, i->height);

	exit(1);
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

	if (outfile == NULL && isatty(1)) {
		fprintf(stderr, "Didn't specify -o and standard output is a terminal\n");
		exit(EXIT_FAILURE);
	}
	FILE *outfp = stdout;
	if (outfile != NULL) {
		outfp = fopen(outfile, "wb");
		if (outfp == NULL) {
			perror(outfile);
			exit(EXIT_FAILURE);
		}
	}
	
	unsigned int x1, y1, x2, y2;
	latlon2tile(maxlat, minlon, 32, &x1, &y1);
	latlon2tile(minlat, maxlon, 32, &x2, &y2);

	unsigned int tx1 = x1 >> (32 - zoom);
	unsigned int ty1 = y1 >> (32 - zoom);
	unsigned int tx2 = x2 >> (32 - zoom);
	unsigned int ty2 = y2 >> (32 - zoom);

	fprintf(stderr, "at zoom level %d, that's %u/%u to %u/%u\n", zoom,
		tx1, ty1, tx2, ty2);

	long long dim = (long long) (tx2 - tx1 + 1) * (ty2 - ty1 + 1) * tilesize * tilesize;
	int width = (tx2 - tx1 + 1) * tilesize;
	int height = (ty2 - ty1 + 1) * tilesize;
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
			int xoff = (tx - tx1) * tilesize;
			int yoff = (ty - ty1) * tilesize;

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
					} else if (cp[1] == 's') {
						*out++ = 'a' + rand() % 3;
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
			fprintf(stderr, "%s\n", url2);

			CURL *curl = curl_easy_init();
			if (curl == NULL) {
				fprintf(stderr, "Curl won't start\n");
				exit(EXIT_FAILURE);
			}

			struct data data;
			data.buf = NULL;
			data.len = 0;
			data.nalloc = 0;

			curl_easy_setopt(curl, CURLOPT_URL, url2);
			curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
			curl_easy_setopt(curl, CURLOPT_WRITEDATA, &data);
			curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_receive);

			CURLcode res = curl_easy_perform(curl);
			if (res != CURLE_OK) {
				fprintf(stderr, "Can't retrieve %s: %s\n", url2,
					curl_easy_strerror(res));
				exit(EXIT_FAILURE);
			}

			struct image *i;

			if (data.len >= 4 && memcmp(data.buf, "\x89PNG", 4) == 0) {
				i = read_png(data.buf, data.len);
			} else if (data.len >= 2 && memcmp(data.buf, "\xFF\xD8", 2) == 0) {
				i = read_jpeg(data.buf, data.len);
			} else {
				fprintf(stderr, "Don't recognize file format\n");
				exit(EXIT_FAILURE);
			}

			free(data.buf);
			curl_easy_cleanup(curl);

			int x, y;
			for (y = 0; y < i->height; y++) {
				for (x = 0; x < i->width; x++) {
					if (i->depth == 4) {
						buf[((y + yoff) * width + x + xoff) * 4 + 0] = i->buf[(y * i->width + x) * 4 + 0];
						buf[((y + yoff) * width + x + xoff) * 4 + 1] = i->buf[(y * i->width + x) * 4 + 1];
						buf[((y + yoff) * width + x + xoff) * 4 + 2] = i->buf[(y * i->width + x) * 4 + 2];
						buf[((y + yoff) * width + x + xoff) * 4 + 3] = i->buf[(y * i->width + x) * 4 + 3];
					} else if (i->depth == 3) {
						buf[((y + yoff) * width + x + xoff) * 4 + 0] = i->buf[(y * i->width + x) * 3 + 0];
						buf[((y + yoff) * width + x + xoff) * 4 + 1] = i->buf[(y * i->width + x) * 3 + 1];
						buf[((y + yoff) * width + x + xoff) * 4 + 2] = i->buf[(y * i->width + x) * 3 + 2];
						buf[((y + yoff) * width + x + xoff) * 4 + 3] = 255;
					} else {
						buf[((y + yoff) * width + x + xoff) * 4 + 0] = i->buf[(y * i->width + x) * i->depth + 0];
						buf[((y + yoff) * width + x + xoff) * 4 + 1] = i->buf[(y * i->width + x) * i->depth + 0];
						buf[((y + yoff) * width + x + xoff) * 4 + 2] = i->buf[(y * i->width + x) * i->depth + 0];
						buf[((y + yoff) * width + x + xoff) * 4 + 3] = 255;
					}
				}
			}

			free(i->buf);
			free(i);
		}
	}

	unsigned char *rows[height];
	for (i = 0 ; i < height; i++) {
		rows[i] = buf + i * (4 * width);
	}

	png_structp png_ptr;
	png_infop info_ptr;

	png_ptr = png_create_write_struct(PNG_LIBPNG_VER_STRING, fail, fail, fail);
	if (png_ptr == NULL) {
		fprintf(stderr, "PNG failure (write struct)\n");
		exit(EXIT_FAILURE);
	}
	info_ptr = png_create_info_struct(png_ptr);
	if (info_ptr == NULL) {
		png_destroy_write_struct(&png_ptr, NULL);
		fprintf(stderr, "PNG failure (info struct)\n");
		exit(EXIT_FAILURE);
	}

	png_set_IHDR(png_ptr, info_ptr, width, height, 8, PNG_COLOR_TYPE_RGB_ALPHA, PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
	png_set_rows(png_ptr, info_ptr, rows);
	png_init_io(png_ptr, outfp);
	png_write_png(png_ptr, info_ptr, 0, NULL);
	png_destroy_write_struct(&png_ptr, &info_ptr);

	if (outfile != NULL) {
		fclose(outfp);
	}

	return 0;
}
