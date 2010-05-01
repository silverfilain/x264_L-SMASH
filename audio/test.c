#include "audio_internal.h"
#include <assert.h>
#include <stdio.h>

extern struct audio_filter_t audio_demux_lavf;

int main( int argc, char **argv )
{
    char *file = "test.avi";
    if( argc > 1 )
        file = argv[1];
    hnd_t handle;
    audio_filter_t *lavf = &audio_demux_lavf;
    assert( lavf->init( lavf, NULL, &handle, file ) == AUDIO_OK );
    audio_hnd_t *h = handle;
    h->self = lavf;
    int64_t sample = 0;
    AVPacket *pkt;
    FILE *f = fopen( "dump", "w" );
    while( ( pkt = lavf->get_samples( h, sample, sample + h->info->framelen / 2 ) ) )
    {
        sample += pkt->size / h->info->samplesize;
        fwrite( pkt->data, 1, pkt->size, f );
        fprintf( stdout, "Read %lld samples\n", sample );
        fflush( stdout );
        lavf->free_packet( handle, pkt );
    }
    fclose( f );
    lavf->close( handle );
}
