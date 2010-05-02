CFLAGS  = -Wall -g -std=gnu99 -I.. `pkg-config --cflags libavformat`
LDFLAGS = -g `pkg-config --libs libavformat`

AUDIO_SOURCES = demux_lavf.o

AUDIO_DEPS = audio.o audio_internal.o \
	$(AUDIO_SOURCES) \
	$(AUDIO_FILTERS) \
	$(AUDIO_ENCODERS) \
	$(AUDIO_MUXERS)

test: test.o $(AUDIO_DEPS)

clean:
	rm *.o test
