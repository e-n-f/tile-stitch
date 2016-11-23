tile-stitch
===========

Stitch together and crop map tiles for any bounding box.

The tiles should come from a web map service in PNG or JPEG format, and will be written out as PNG.

Examples
--------

To get standard OpenStreetMap tiles at zoom level 10 for the area of the Exploratorium's Bay Model video projection:

    $ ./stitch -o baymodel.png -- 37.371794 -122.917099 38.226853 -121.564407 10 http://a.tile.openstreetmap.org/{z}/{x}/{y}.png

To get the MapQuest Open Aerial imagery at zoom level 11 to match the "See Something or Say Something" bounding box of London:

    $ ./stitch -o london.png -- 51.316252 -0.366258 51.606525 0.099606 11 http://otile1.mqcdn.com/tiles/1.0.0/sat/{z}/{x}/{y}.jpg

To get a 640x480 image from Stamen's watercolor map at zoom level 10 around Tokyo:

    $ ./stitch -o tokyo.png -c -- 35.6824 139.7531 640 480 10 http://b.tile.stamen.com/watercolor/{z}/{x}/{y}.jpg
    
To get 512x512 retina tiles of Stamen's Toner tiles at zoom level 14 around Köln:

    $ ./stitch -o köln.png -t 512 -- 50.88 6.88 50.98 7.04 14 http://b.tile.stamen.com/toner/{z}/{x}/{y}@2x.png

Format
------

The arguments are <i>minlat minlon maxlat maxlon zoom url</i>. If you don't specify <i>-o outfile</i> the PNG will be
written to the standard output. URLs should include <i>{z}, {x},</i> and <i>{y}</i> tokens for tile zoom, x, and y.

The <code>--</code> is to keep getopt, especially GNU getopt, from interpreting the minus signs in latitudes or longitudes
as option flags.

Requirements
------------

  * pkg-config
  * libcurl to retrieve the tiles
  * libjpeg >= 8 for jpeg_mem_src
  * libpng

Installation
------------

To install on Ubuntu do:

    sudo apt-get update
    sudo apt-get install git build-essential pkg-config libcurl4-openssl-dev libpng-dev libjpeg-dev
    git clone git@github.com:ericfischer/tile-stitch.git
    cd tile-stitch
    make
