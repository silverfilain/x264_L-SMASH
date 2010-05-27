#include "audio/audio.h"
#include "audio/muxers.h"
#include "filters/audio/internal.h"
#include <assert.h>
#include <stdio.h>
#include <inttypes.h>

extern struct audio_filter_t audio_demux_lavf;
extern struct audio_muxer_t audio_muxer_raw;

int main( int argc, char **argv )
{
    char *file = "test.avi";
    if( argc > 1 )
        file = argv[1];
    hnd_t h = audio_open_from_file( NULL, file, TRACK_ANY );
    if( !h )
        exit( 1 );
    hnd_t m = audio_muxer_raw.init( &audio_muxer_raw, h, "dump" );
    if( !m )
    {
        af_close( h );
        exit( 2 );
    }

    int64_t i;
    while( audio_muxer_raw.write_audio( m, i++ ) >= 0 )
        ;

    audio_muxer_raw.close( m );
    af_close( h );
}
