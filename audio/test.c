#include "audio/audio.h"
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
    if( !af_add( h, af_get_filter( AUDIO_MUXER_RAW ), "dump" ) )
    {
        af_close( h );
        exit( 2 );
    }

    int64_t sampleno = 0;
    int64_t samplelen = 4096;
    audio_samples_t samples = {};
    
    while( !(samples.flags & AUDIO_FLAG_EOF) && af_get_samples( &samples, h, sampleno, sampleno + samplelen ) )
    {
        sampleno += samples.samplecount;
        fprintf( stdout, "Read %"PRIu64" samples (flags: %d)\n", sampleno, samples.flags );
        fflush( stdout );
        af_free_samples( &samples );
    }
    af_close( h );
}
