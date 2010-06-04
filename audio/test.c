#include "audio/audio.h"
#include "audio/encoders.h"
#include "filters/audio/internal.h"
#include <assert.h>
#include <stdio.h>
#include <inttypes.h>

extern struct audio_filter_t audio_demux_lavf;

int main( int argc, char **argv )
{
    char *file = "test.avi";
    if( argc > 1 )
        file = argv[1];
    hnd_t h = audio_open_from_file( NULL, file, TRACK_ANY );
    if( !h )
        exit( 1 );
    hnd_t enc = audio_encoder_open( &audio_encoder_raw, h, NULL );
    
	FILE *f = fopen( "dump", "wb" );
	audio_samples_t *samples;
	while( ( samples = audio_encode_frame( enc ) ) ) {
	    fwrite( samples->data, 1, samples->len, f );
	    audio_free_frame( enc, samples );
	}
	fclose( f );

    audio_encoder_close( enc );
    af_close( h );
}
