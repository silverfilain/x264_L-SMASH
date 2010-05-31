#include "audio/audio.h"
#include "audio/encoders.h"
#include "filters/audio/internal.h"
#include <assert.h>
#include <stdio.h>
#include <inttypes.h>

extern struct audio_filter_t audio_demux_lavf;
extern struct audio_encoder_t aenc_raw;

int main( int argc, char **argv )
{
    char *file = "test.avi";
    if( argc > 1 )
        file = argv[1];
    hnd_t h = audio_open_from_file( NULL, file, TRACK_ANY );
    if( !h )
        exit( 1 );
    hnd_t enc = aenc_raw.init( h, NULL );
    
	FILE *f = fopen( "dump", "wb" );
	audio_samples_t *samples;
	while( ( samples = aenc_raw.get_next_packet( enc ) ) ) {
	    fwrite( samples->data, 1, samples->len, f );
	    aenc_raw.free_packet( enc, samples );
	}
	fclose( f );

    aenc_raw.close( enc );
    af_close( h );
}
