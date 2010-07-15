#include "filters/audio/internal.h"

#include <assert.h>

hnd_t audio_open_from_file( audio_filter_t *preferred_filter, char *path, int trackno )
{
    audio_filter_t *source = preferred_filter ? preferred_filter : af_get_filter( "lavfsource" );
    if( !source )
    {
        x264_cli_log( "audio", X264_LOG_ERROR, "no decoder / demuxer avilable!\n" );
        return NULL;
    }
    hnd_t h = NULL;
    size_t init_arg_size = strlen( path ) + 10;
    char *init_arg = malloc( init_arg_size );
    assert( snprintf( init_arg, init_arg_size, "%s,%d", path, trackno ) < init_arg_size );
    if( source->init( &h, init_arg ) < 0 || !h )
    {
        x264_cli_log( "audio", X264_LOG_ERROR, "error initializing source filter!\n" );
        return NULL;
    }
    free( init_arg );
    return h;
}
