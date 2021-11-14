#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include <curl/curl.h>
#include <jpeglib.h>
#include <png.h>
#include <geotiffio.h>
#include <xtiffio.h>

void usage(char **argv) {
	fprintf(stderr, "Usage: %s [-o outfile] [-f png|geotiff] minlat minlon maxlat maxlon zoom http://whatever/{z}/{x}/{y}.png ...\n", argv[0]);
	fprintf(stderr, "Usage: %s [-o outfile] [-f png|geotiff] -c lat lon width height zoom http://whatever/{z}/{x}/{y}.png ...\n", argv[0]);
}

// http://wiki.openstreetmap.org/wiki/Slippy_map_tilenames
void latlon2tile(double lat, double lon, int zoom, unsigned int *x, unsigned int *y) {
	double lat_rad = lat * M_PI / 180;
	unsigned long long n = 1LL << zoom;

	*x = n * ((lon + 180) / 360);
	*y = n * (1 - (log(tan(lat_rad) + 1 / cos(lat_rad)) / M_PI)) / 2;
}

// http://wiki.openstreetmap.org/wiki/Slippy_map_tilenames
void tile2latlon(unsigned int x, unsigned int y, int zoom, double *lat, double *lon) {
	unsigned long long n = 1LL << zoom;
	*lon = 360.0 * x / n - 180.0;
	double lat_rad = atan(sinh(M_PI * (1 - 2.0 * y / n)));
	*lat = lat_rad * 180 / M_PI;
}

// Convert lat/lon in WGS84 to XY in Spherical Mercator (EPSG:900913/3857)
void projectlatlon(double lat, double lon, double *x, double *y) {
	static const double originshift = 20037508.342789244;  // 2 * pi * 6378137 / 2
	*x = lon * originshift / 180.0;
	*y = log(tan((90 + lat) * M_PI / 360.0)) / (M_PI / 180.0);
	*y = *y * originshift / 180.0;
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

enum outfileformat { OUTFMT_PNG,
		     OUTFMT_GEOTIFF };

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
	JSAMPARRAY buffer = (*cinfo.mem->alloc_sarray)((j_common_ptr) &cinfo, JPOOL_IMAGE, row_stride, 1);

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
	long long i;

	char *outfile = NULL;
	int tilesize = 256;
	int centered = 0;
	int outfmt = OUTFMT_PNG;
	unsigned int writeworldfile = FALSE;
	while ((i = getopt(argc, argv, "o:t:cf:ws:")) != -1) {
		switch (i) {
		case 'o':
			outfile = optarg;
			break;

		case 't':
			tilesize = atoi(optarg);
			break;

		case 'c':
			centered = 1;
			break;

		case 'w':
			writeworldfile = TRUE;
			break;

		case 's':
			tilesize = atoi(optarg);
			break;

		case 'f':
			if (strcmp(optarg, "png") == 0) {
				outfmt = OUTFMT_PNG;
			} else if (strcmp(optarg, "geotiff") == 0) {
				outfmt = OUTFMT_GEOTIFF;
			}
			break;

		default:
			usage(argv);
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
		fprintf(stderr, "Zoom %u less than 0\n", zoom);
		exit(EXIT_FAILURE);
	}

	if (outfile == NULL && isatty(1)) {
		fprintf(stderr, "Didn't specify -o and standard output is a terminal\n");
		exit(EXIT_FAILURE);
	}

	unsigned int x1, y1, x2, y2;

	if (centered) {
		latlon2tile(minlat, minlon, 32, &x1, &y1);
		latlon2tile(minlat, minlon, 32, &x2, &y2);

		int width = atoi(argv[optind + 2]);
		int height = atoi(argv[optind + 3]);

		if (width <= 0 || height <= 0) {
			fprintf(stderr, "Width/height less than 0: %u %u\n", width, height);
			exit(EXIT_FAILURE);
		}

		x1 = x1 - (width << (32 - (zoom + 8))) / 2;
		y1 = y1 - (height << (32 - (zoom + 8))) / 2;
		x2 = x2 + (width << (32 - (zoom + 8))) / 2;
		y2 = y2 + (height << (32 - (zoom + 8))) / 2;

		tile2latlon(x1, y1, 32, &maxlat, &minlon);
		tile2latlon(x2, y2, 32, &minlat, &maxlon);
	} else {
		latlon2tile(maxlat, minlon, 32, &x1, &y1);
		latlon2tile(minlat, maxlon, 32, &x2, &y2);
	}

	unsigned int tx1 = x1 >> (32 - zoom);
	unsigned int ty1 = y1 >> (32 - zoom);
	unsigned int tx2 = x2 >> (32 - zoom);
	unsigned int ty2 = y2 >> (32 - zoom);

	double miny, minx, maxy, maxx;
	projectlatlon(minlat, minlon, &minx, &miny);
	projectlatlon(maxlat, maxlon, &maxx, &maxy);

	fprintf(stderr, "==Geodetic Bounds  (EPSG:4236): %.17g,%.17g to %.17g,%.17g\n", minlat, minlon, maxlat, maxlon);
	fprintf(stderr, "==Projected Bounds (EPSG:3785): %.17g,%.17g to %.17g,%.17g\n", miny, minx, maxy, maxx);
	fprintf(stderr, "==Zoom Level: %u\n", zoom);
	fprintf(stderr, "==Upper Left Tile: x:%u y:%u\n", tx1, ty2);
	fprintf(stderr, "==Lower Right Tile: x:%u y:%u\n", tx2, ty1);

	unsigned int xa = ((x1 >> (32 - (zoom + 8))) & 0xFF) * tilesize / 256;
	unsigned int ya = ((y1 >> (32 - (zoom + 8))) & 0xFF) * tilesize / 256;

	int width = ((x2 >> (32 - (zoom + 8))) - (x1 >> (32 - (zoom + 8)))) * tilesize / 256;
	int height = ((y2 >> (32 - (zoom + 8))) - (y1 >> (32 - (zoom + 8)))) * tilesize / 256;
	fprintf(stderr, "==Raster Size: %ux%u\n", width, height);

	double px = (maxx - minx) / width;
	double py = (fabs(maxy - miny)) / height;
	fprintf(stderr, "==Pixel Size: x:%.17g y:%.17g\n", px, py);

	long long dim = (long long) width * height;
	if (dim > 35000 * 35000) {
		fprintf(stderr, "that's too big\n");
		exit(EXIT_FAILURE);
	}

	unsigned char *buf = malloc(dim * 4);
	memset(buf, '\0', dim * 4);
	if (buf == NULL) {
		fprintf(stderr, "Can't allocate memory for %lld\n", dim * 4);
		exit(EXIT_FAILURE);
	}

	unsigned int tx, ty;
	for (ty = ty1; ty <= ty2; ty++) {
		for (tx = tx1; tx <= tx2; tx++) {
			double progress = ((double) ty - ty1) / ((ty2 + 1) - ty1) +
					  ((double) tx - tx1) / ((ty2 + 1) - ty1) / ((tx2 + 1) - tx1);

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
				fprintf(stderr, "%0.2f%%: %s\n", progress * 100, url2);

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
				curl_easy_setopt(curl, CURLOPT_USERAGENT, "tile-stitch/1.0.0");
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
					/* if the tile is missing, don't warn */
					if (data.len != 0)
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

				long long x, y;
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
							buf[((y + yoff) * width + x + xoff) * 4 + 0] = rr / ar * 255.0;
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
	for (i = 0; i < height; i++) {
		rows[i] = buf + i * (4 * width);
	}

	if (outfmt == OUTFMT_PNG) {
		FILE *outfp = stdout;
		if (outfile != NULL) {
			fprintf(stderr, "Output PNG: %s\n", outfile);
			outfp = fopen(outfile, "wb");
			if (outfp == NULL) {
				perror(outfile);
				exit(EXIT_FAILURE);
			}
		} else
			fprintf(stderr, "Output PNG: stdout\n");
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
	}

	else if (outfmt == OUTFMT_GEOTIFF) {
		//TODO : Handle writing to stdout if required

		if (outfile != NULL) {
			fprintf(stderr, "Output TIFF: %s\n", outfile);
			TIFF *tif = (TIFF *) 0;  /* TIFF-level descriptor */
			GTIF *gtif = (GTIF *) 0; /* GeoKey-level descriptor */

			tif = XTIFFOpen(outfile, "w");
			if (!tif) {
				fprintf(stderr, "TIF failure (open)\n");
				exit(EXIT_FAILURE);
			}

			gtif = GTIFNew(tif);
			if (!gtif) {
				printf("GTIFF failure (geotiff struct)\n");
				exit(EXIT_FAILURE);
			}

			//georeference the image using the upper left projected bound
			//as a tie point, and the pixel scale
			double pixscale[3] = {px, py, 0};
			double tiepoints[6] = {0, 0, 0, minx, maxy, 0.0};
			TIFFSetField(tif, TIFFTAG_GEOPIXELSCALE, 3, pixscale);
			TIFFSetField(tif, TIFFTAG_GEOTIEPOINTS, 6, tiepoints);

			TIFFSetField(tif, TIFFTAG_IMAGEWIDTH, width);
			TIFFSetField(tif, TIFFTAG_IMAGELENGTH, height);
			TIFFSetField(tif, TIFFTAG_COMPRESSION, COMPRESSION_LZW);
			TIFFSetField(tif, TIFFTAG_PREDICTOR, 2);  //(horizontal differencing)
			TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, 8);
			TIFFSetField(tif, TIFFTAG_ROWSPERSTRIP, 20L);
			TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, 4);  //RGB+ALPHA
			TIFFSetField(tif, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
			TIFFSetField(tif, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_RGB);

			uint16 out[1];
			out[0] = EXTRASAMPLE_ASSOCALPHA;
			TIFFSetField(tif, TIFFTAG_EXTRASAMPLES, 1, &out );

			GTIFKeySet(gtif, GTModelTypeGeoKey, TYPE_SHORT, 1, ModelTypeProjected);
			GTIFKeySet(gtif, GTRasterTypeGeoKey, TYPE_SHORT, 1, RasterPixelIsArea);
			GTIFKeySet(gtif, GTCitationGeoKey, TYPE_ASCII, 0, "WGS 84 / Pseudo-Mercator");
			GTIFKeySet(gtif, GeogCitationGeoKey, TYPE_ASCII, 0, "WGS 84");
			GTIFKeySet(gtif, GeogAngularUnitsGeoKey, TYPE_SHORT, 1, Angular_Degree);
			GTIFKeySet(gtif, GeogLinearUnitsGeoKey, TYPE_SHORT, 1, Linear_Meter);
			GTIFKeySet(gtif, ProjectedCSTypeGeoKey, TYPE_SHORT, 1, 3857);

			//write raster image
			for (i = 0; i < height; i++) {
				if (!TIFFWriteScanline(tif, rows[i], i, 0)) {
					TIFFError("WriteImage", "failure in WriteScanline\n");
					exit(EXIT_FAILURE);
				}
			}

			GTIFWriteKeys(gtif);
			GTIFFree(gtif);
			XTIFFClose(tif);
		} else {
			fprintf(stderr, "Can't write TIFF to stdout, sorry\n");
			exit(EXIT_FAILURE);
		}
	}

	//write world file
	if (writeworldfile) {
		if (outfile != NULL) {
			char worldfile_filename[1024];
			char worldfilext[5];
			double wfvals[6];
			FILE *fp;

			//todo, make sure the output image file has the right extension
			if (outfmt == OUTFMT_PNG) {
				snprintf(worldfilext, sizeof worldfilext, ".pnw");
			} else if (outfmt == OUTFMT_GEOTIFF) {
				snprintf(worldfilext, sizeof worldfilext, ".tfw");
			}

			strncpy(worldfile_filename, outfile, sizeof(worldfile_filename) - 4);
			for (i = strlen(worldfile_filename) - 1; i > 0; i--) {
				if (worldfile_filename[i] == '.') {
					strcpy(worldfile_filename + i, worldfilext);
					break;
				}
			}
			if (i <= 0) {
				strcat(worldfile_filename, worldfilext);
			}

			wfvals[0] = px;    // x pixel resolution
			wfvals[1] = 0;     // rotation
			wfvals[2] = 0;     // rotation
			wfvals[3] = -py;   // y pix resolution - negative as y direction is inverse of raster
			wfvals[4] = minx;  // top left x
			wfvals[5] = maxy;  // top left y

			fp = fopen(worldfile_filename, "wt");
			if (fp == NULL) {
				fprintf(stderr, "Failed to open World File `%s'\n", worldfile_filename);
				exit(EXIT_FAILURE);
			}

			for (i = 0; i < 6; i++) {
				fprintf(fp, "%24.10f\n", wfvals[i]);
			}

			fclose(fp);
			fprintf(stderr, "World file written to '%s'.\n", worldfile_filename);
		} else {
			fprintf(stderr, "Can't write a worldfile when writing to stdout\n");
		}
	}
	return 0;
}
