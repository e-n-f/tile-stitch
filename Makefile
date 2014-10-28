CC := $(CC)
CXX := $(CXX)
CXXFLAGS := $(CXXFLAGS)
LDFLAGS := $(LDFLAGS)

CURL_CFLAGS := $(shell pkg-config --cflags libcurl)
PNG_CFLAGS := $(shell pkg-config --cflags libpng)
CURL_LIBS := $(shell pkg-config --libs libcurl)
PNG_LIBS := $(shell pkg-config --libs libpng)

stitch: stitch.c
	$(CC) -g -Wall -O3 $(CFLAGS) $(LDFLAGS) -o stitch stitch.c $(CURL_CFLAGS) $(PNG_CFLAGS) $(JPEG_CFLAGS) $(CURL_LIBS) $(PNG_LIBS) $(JPEG_LIBS) -ljpeg -lm

clean:
	rm -f stitch
