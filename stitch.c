#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include <curl/curl.h>
#include <jpeglib.h>
#include <png.h>

void usage(char **argv) {
	fprintf(stderr, "Usage: %s [-o outfile] minlat minlon maxlat maxlon zoom http://whatever/{z}/{x}/{y}.png ...\n", argv[0]);
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

struct read_state {
	char *base;
	int off;
	int len;
};

void user_read_data(png_structp png_ptr, png_bytep data, png_size_t length) {
	struct read_state *state = png_get_io_ptr(png_ptr);

	if (state->off + length > state->len) {
		length = state->len - state->off;
	}

	memcpy(data, state->base + state->off, length);
	state->off += length;
}

struct image *read_png(char *s, int len) {
	png_structp png_ptr;
	png_infop info_ptr;

	struct read_state state;
	state.base = s;
	state.off = 0;
	state.len = len;

	png_ptr = png_create_read_struct(PNG_LIBPNG_VER_STRING, fail, fail, fail);
	if (png_ptr == NULL) {
		fprintf(stderr, "PNG init failed\n");
		exit(EXIT_FAILURE);
	}

	info_ptr = png_create_info_struct(png_ptr);
	if (info_ptr == NULL) {
		fprintf(stderr, "PNG init failed\n");
		exit(EXIT_FAILURE);
	}

	png_set_read_fn(png_ptr, &state, user_read_data);
	png_set_sig_bytes(png_ptr, 0);

	png_read_png(png_ptr, info_ptr, PNG_TRANSFORM_STRIP_16 | PNG_TRANSFORM_PACKING | PNG_TRANSFORM_EXPAND, NULL);

	png_uint_32 width, height;
	int bit_depth;
	int color_type, interlace_type;

	png_get_IHDR(png_ptr, info_ptr, &width, &height, &bit_depth, &color_type, &interlace_type, NULL, NULL);

	struct image *i = malloc(sizeof(struct image));
	i->width = width;
	i->height = height; 
	i->depth = png_get_channels(png_ptr, info_ptr);
	i->buf = malloc(i->width * i->height * i->depth);

	unsigned int row_bytes = png_get_rowbytes(png_ptr, info_ptr);
	png_bytepp row_pointers = png_get_rows(png_ptr, info_ptr);

	int n;
	for (n = 0; n < i->height; n++) {
		memcpy(i->buf + row_bytes * n, row_pointers[n], row_bytes);
	}

	png_destroy_read_struct(&png_ptr, &info_ptr, NULL);
	return i;
}

int main(int argc, char **argv) {
	extern int optind;
	extern char *optarg;
	int i;

	char *outfile = NULL;
	int tilesize = 256;

	while ((i = getopt(argc, argv, "o:t:")) != -1) {
		switch (i) {
		case 'o':
			outfile = optarg;
			break;

		case 't':
			tilesize = atoi(optarg);
			break;

		default:
			usage(argv);
			exit(EXIT_FAILURE);
		}
	}

	if (argc - optind < 6) {
		usage(argv);
		exit(EXIT_FAILURE);
	}

	double minlat = atof(argv[optind]);
	double minlon = atof(argv[optind + 1]);
	double maxlat = atof(argv[optind + 2]);
	double maxlon = atof(argv[optind + 3]);
	int zoom = atoi(argv[optind + 4]);

	if (zoom < 0) {
		fprintf(stderr, "Zoom %d less than 0\n", zoom);
		exit(EXIT_FAILURE);
	}

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

	unsigned int xa = ((x1 >> (32 - (zoom + 8))) & 0xFF) * tilesize / 256;
	unsigned int ya = ((y1 >> (32 - (zoom + 8))) & 0xFF) * tilesize / 256;

	unsigned int xb = (255 - ((x2 >> (32 - (zoom + 8))) & 0xFF)) * tilesize / 256;
	unsigned int yb = (255 - ((y2 >> (32 - (zoom + 8))) & 0xFF)) * tilesize / 256;

	fprintf(stderr, "at zoom level %d, that's %u/%u to %u/%u\n", zoom,
		tx1, ty1, tx2, ty2);

	fprintf(stderr, "borders %u,%u %u,%u\n", xa, ya, xb, yb);

	int width = (tx2 - tx1 + 1) * tilesize - xa - xb;
	int height = (ty2 - ty1 + 1) * tilesize - ya - yb;
	fprintf(stderr, "%dx%d\n", width, height);

	long long dim = (long long) width * height;
	if (dim > 10000 * 10000) {
		fprintf(stderr, "that's too big\n");
		exit(EXIT_FAILURE);
	}

	unsigned char *buf = malloc(dim * 4);
	memset(buf, '\0', dim * 4);
	if (buf == NULL) {
		fprintf(stderr, "Can't allocate memory for %lld\n", dim * 4);
	}
	
	unsigned int tx, ty;
	for (tx = tx1; tx <= tx2; tx++) {
		for (ty = ty1; ty <= ty2; ty++) {
			int xoff = (tx - tx1) * tilesize - xa;
			int yoff = (ty - ty1) * tilesize - ya;

			int opt;
			for (opt = optind + 5; opt < argc; opt++) {
				char *url = argv[opt];

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

					free(data.buf);
					curl_easy_cleanup(curl);
					continue;
				}

				free(data.buf);
				curl_easy_cleanup(curl);

				if (i->height != tilesize || i->width != tilesize) {
					fprintf(stderr, "Got %dx%d tile, not %d\n", i->width, i->height, tilesize);
					exit(EXIT_FAILURE);
				}

				int x, y;
				for (y = 0; y < i->height; y++) {
					for (x = 0; x < i->width; x++) {
						int xd = x + xoff;
						int yd = y + yoff;

						if (xd < 0 || yd < 0 || xd >= width || yd >= height) {
							continue;
						}

						if (i->depth == 4) {
							double as = buf[((y + yoff) * width + x + xoff) * 4 + 3] / 255.0;
							double rs = buf[((y + yoff) * width + x + xoff) * 4 + 0] / 255.0 * as;
							double gs = buf[((y + yoff) * width + x + xoff) * 4 + 1] / 255.0 * as;
							double bs = buf[((y + yoff) * width + x + xoff) * 4 + 2] / 255.0 * as;

							double ad = i->buf[(y * i->width + x) * 4 + 3] / 255.0;
							double rd = i->buf[(y * i->width + x) * 4 + 0] / 255.0 * ad;
							double gd = i->buf[(y * i->width + x) * 4 + 1] / 255.0 * ad;
							double bd = i->buf[(y * i->width + x) * 4 + 2] / 255.0 * ad;

							// https://code.google.com/p/pulpcore/wiki/TutorialBlendModes
							double ar = as * (1 - ad) + ad;
							double rr = rs * (1 - ad) + rd;
							double gr = gs * (1 - ad) + gd;
							double br = bs * (1 - ad) + bd;

							buf[((y + yoff) * width + x + xoff) * 4 + 3] = ar * 255.0;
							buf[((y + yoff) * width + x + xoff) * 4 + 0] = rr / ar* 255.0;
							buf[((y + yoff) * width + x + xoff) * 4 + 1] = gr / ar * 255.0;
							buf[((y + yoff) * width + x + xoff) * 4 + 2] = br / ar * 255.0;
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
