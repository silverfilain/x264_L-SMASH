CFLAGS  = -Wall -g -std=gnu99 -I../.. `pkg-config --cflags libavformat`
LDFLAGS = -g `pkg-config --libs libavformat`

AUDIO_SOURCES = source_lavf.o
AUDIO_MUXERS  = muxer_raw.o

AUDIO_DEPS = audio.o audio_internal.o \
	../common.o \
	$(AUDIO_SOURCES) \
	$(AUDIO_FILTERS) \
	$(AUDIO_ENCODERS) \
	$(AUDIO_MUXERS)

test: test.o $(AUDIO_DEPS)

clean:
	rm *.o ../*.o test
