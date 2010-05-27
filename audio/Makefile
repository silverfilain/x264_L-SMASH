CFLAGS  = -Wall -g -std=gnu99 -I.. `pkg-config --cflags libavformat`
LDFLAGS = -g `pkg-config --libs libavformat`

AUDIO_DEPS = audio.o \
	muxers/muxer_raw.o \
	../filters/common.o \
	../filters/audio/audio_filters.o \
	../filters/audio/internal.o \
	../filters/audio/source_lavf.o \

test: test.o $(AUDIO_DEPS)

clean:
	rm *.o ../*.o test
