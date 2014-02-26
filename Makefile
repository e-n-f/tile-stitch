CFLAGS = -I$(HOME)/include
LDFLAGS = -L$(HOME)/lib

stitch: stitch.c
	cc -g -Wall -O3 $(CFLAGS) $(LDFLAGS) -o stitch stitch.c -lcurl -lm -ljpeg
