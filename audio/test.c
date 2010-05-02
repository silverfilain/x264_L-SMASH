#include "audio_internal.h"
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

    int64_t sampleno = 0;
    int64_t samplelen = 4096;
    audio_samples_t samples = {};
    
    FILE *f = fopen( "dump", "w" );
    while( !(samples.flags & AUDIO_FLAG_EOF) && audio_filter_samples( &samples, h, sampleno, sampleno + samplelen ) )
    {
        sampleno += samples.samplecount;
        fwrite( samples.data, 1, samples.len, f );
        fprintf( stdout, "Read %"PRIu64" samples (flags: %d)\n", sampleno, samples.flags );
        fflush( stdout );
        audio_free_samples( &samples );
    }
    fclose( f );
    audio_close( h );
}
