#include "audio/audio_internal.h"
#include <assert.h>
#include <stdio.h>
#include <inttypes.h>

typedef struct lavf_source_t
{
    AUDIO_FILTER_COMMON
    AVFormatContext *lavf;
    AVCodecContext *ctx;
    AVCodec *codec;

    unsigned track;
    uint8_t *buffer;
    intptr_t bufsize;
    intptr_t surplus;
    intptr_t len;
    uint64_t bytepos;
} lavf_source_t;

#define DEFAULT_BUFSIZE AVCODEC_MAX_AUDIO_FRAME_SIZE * 2

static int buffer_next_frame( lavf_source_t *h );

static enum AudioResult init( const struct audio_filter_t *self, hnd_t base, hnd_t *handle, const char *opt_str )
{
    assert( self );
    assert( opt_str );
    assert( !base ); // This must be the first filter
    assert( handle );
    char *optlist[] = { "filename", "track", NULL };
    char **opts     = split_options( opt_str, optlist );

    char *filename = get_option( "filename", opts );
    char *trackstr = get_option( "track", opts );

    assert( filename );
    int track;
    if( !trackstr )
        track = TRACK_ANY;
    else if ( !strcmp( trackstr, "any" ) )
        track = TRACK_ANY;
    else
        track = atoi( trackstr );

    FILE *test = fopen( filename, "r" );
    if( test )
        fclose( test );
    else
    {
        fprintf( stderr, "lavfsource [error]: Could not open '%s' for reading\n", filename );
        return AUDIO_ERROR;
    }
    lavf_source_t *h = *handle = calloc( 1, sizeof( lavf_source_t ) );
    h->self = (audio_filter_t*) self;

    av_register_all();
    if( !strcmp( filename, "-" ) )
        filename = "pipe:";

    if( av_open_input_file( &h->lavf, filename, NULL, 0, NULL ) )
    {
        fprintf( stderr, "lavfsource [error]: could not open audio file\n" );
        goto fail;
    }

    if( av_find_stream_info( h->lavf ) < 0 )
    {
        fprintf( stderr, "lavfsource [error]: could not find stream info\n" );
        goto fail;
    }

    unsigned tid = TRACK_NONE;
    if( track >= 0 )
    {
        if( track < h->lavf->nb_streams &&
                h->lavf->streams[track]->codec->codec_type == CODEC_TYPE_AUDIO )
            tid = track;
        else
            fprintf( stderr,
                     "lavfsource [error]: requested track %d is unavailable "
                     "or is not an audio track\n", track );
    }
    else // TRACK_ANY (pick first)
    {
        for( track = 0;
                track < h->lavf->nb_streams &&
                h->lavf->streams[track]->codec->codec_type != CODEC_TYPE_AUDIO; )
            ++track;
        if( track < h->lavf->nb_streams )
            tid = track;
        else
            fprintf( stderr, "lavfsource [error]: could not find any audio track\n" );
    }

    if( tid == TRACK_NONE )
        goto fail;

    h->track = tid;

    h->ctx = h->lavf->streams[tid]->codec;
    h->codec = avcodec_find_decoder( h->ctx->codec_id );
    if( avcodec_open( h->ctx, h->codec ) )
        goto codecfail;

    h->info = calloc( 1, sizeof( audio_info_t ) );

    h->info->samplerate = h->ctx->sample_rate;
    h->info->samplefmt = h->ctx->sample_fmt;
    h->info->samplesize = ( av_get_bits_per_sample_format( h->ctx->sample_fmt ) * h->ctx->channels ) / 8;
    h->info->channels = h->ctx->channels;
    h->info->chanlayout = h->ctx->channel_layout;
    h->info->framelen = h->ctx->frame_size;
    h->info->framesize = h->ctx->frame_size * h->info->samplesize;
    h->info->time_base = h->ctx->time_base;
    h->info->extradata = h->ctx->extradata;
    h->info->extradata_size = h->ctx->extradata_size;

    h->bufsize = DEFAULT_BUFSIZE;
    h->surplus = h->info->framesize * 3 / 2;
    assert( h->bufsize > h->surplus * 2 );
    h->buffer = av_malloc( h->bufsize );

    if( !buffer_next_frame( h ) )
        goto codecfail;

    free_string_array( opts );
    return AUDIO_OK;

codecfail:
    fprintf( stderr, "lavfsource [error]: error opening the %s decoder for track %d\n", h->codec->name, h->track );
fail:
    free_string_array( opts );
    if( h->lavf )
        av_close_input_file( h->lavf );
    free( *handle );
    *handle = NULL;
    return AUDIO_ERROR;
}

static inline void free_avpacket( struct AVPacket *pkt )
{
    if( pkt )
    {
        if( pkt->data )
            av_free_packet( pkt );
        free( pkt );
    }
}

static int free_packet( hnd_t handle, struct AVPacket *frame )
{
    free_avpacket( frame );
    return 0;
}

static struct AVPacket *next_packet( lavf_source_t *h )
{
    AVPacket *pkt = calloc( 1, sizeof( AVPacket ) );

    do
    {
        if( pkt->data )
            av_free_packet( pkt );
        if( av_read_frame( h->lavf, pkt ) )
        {
            fprintf( stderr, "lavfsource [info]: end of file reached or read error\n" );
            free_avpacket( pkt );
            return NULL;
        }
    }
    while( pkt->stream_index != h->track );

    return pkt;
}

static int low_decode_audio( lavf_source_t *h, uint8_t *buf, intptr_t buflen )
{
    static AVPacket pkt_temp;
    static AVPacket *pkt = NULL;
    static uint8_t desync_warn = 0;

    int len = 0, datalen = 0;

    while( pkt && pkt_temp.size > 0 )
    {
        datalen = buflen;
        len = avcodec_decode_audio3( h->ctx, (int16_t*) buf, &datalen, &pkt_temp);

        if( len < 0 ) {
            // Broken frame, drop
            if( !desync_warn++ ) // repeat the warning every 256 errors
                fprintf( stderr,
                        "lavfsource [warning]: Decoding errors may cause audio desync\n" );
            pkt_temp.size = 0;
            break;
        }

        pkt_temp.data += len;
        pkt_temp.size -= len;

        if( datalen < 0 )
            continue;

        return datalen;
    }

    free_avpacket( pkt );
    pkt = next_packet( h );

    if( !pkt )
        return -1;

    pkt_temp.data = pkt->data;
    pkt_temp.size = pkt->size;

    return 0;
}

static struct AVPacket *decode_next_frame( lavf_source_t *h )
{
    AVPacket *dst = calloc( 1, sizeof( AVPacket ) );
    assert( !av_new_packet( dst, AVCODEC_MAX_AUDIO_FRAME_SIZE ) );

    int len = 0;
    while( ( len = low_decode_audio( h, dst->data, dst->size ) ) == 0 )
    {
        // Read more
    }
    if( len < 0 ) // EOF or demuxing error
    {
        free_avpacket( dst );
        return NULL;
    }

    dst->size = len;

    return dst;
}

static int buffer_next_frame( lavf_source_t *h )
{
    AVPacket *dec = decode_next_frame( h );
    if( !dec )
        return 0;

    if( h->len + dec->size > h->bufsize )
    {
        memmove( h->buffer, h->buffer + dec->size, h->bufsize - dec->size );
        h->len -= dec->size;
        h->bytepos += dec->size;
    }
    memcpy( h->buffer + h->len, dec->data, dec->size );
    h->len += dec->size;

    free_avpacket( dec );

    return 1;
}

static inline int not_in_cache( lavf_source_t *h, int64_t sample )
{
    int64_t samplebyte = sample * h->info->samplesize;
    if( samplebyte < h->bytepos )
        return -1; // before
    else if( samplebyte < h->bytepos + h->len )
        return 0; // in cache
    return 1; // after
}

static int64_t fill_buffer_until( lavf_source_t *h, int64_t lastsample )
{
    static int errored = 0;
    if( errored )
        return -1;
    if( not_in_cache( h, lastsample ) < 0 )
    {
        fprintf( stderr,
                "lavfsource [error]: backwards seeking not supported "
                "(requested sample %"PRIu64", first available is %ld)\n",
                lastsample, h->bytepos / h->info->samplesize );
        return -1;
    }
    int ret;
    while( ( ret = not_in_cache( h, lastsample ) ) > 0 )
    {
        if( !buffer_next_frame( h ) )
        {
            // libavcodec already warns for us
            errored = 1;
            break;
        }
    }
    assert( ret >= 0 );
    return h->bytepos + h->len;
}


static struct AVPacket *get_samples( hnd_t handle, int64_t first_sample, int64_t last_sample )
{
    lavf_source_t *h = handle;
    assert( first_sample >= 0 && last_sample > first_sample );

    if( fill_buffer_until( h, first_sample ) < 0 )
        return NULL;

    AVPacket *pkt = calloc( 1, sizeof( AVPacket ) );
    assert( !av_new_packet( pkt, ( last_sample - first_sample ) * h->info->samplesize ) );

    if( pkt->size + h->surplus > h->bufsize )
    {
        int64_t pivot = first_sample + ( h->bufsize - h->surplus * 2 ) / h->info->samplesize;
        int64_t expected_size = ( pivot - first_sample ) * h->info->samplesize;

        AVPacket *prev = get_samples( h, first_sample, pivot );
        if( !prev )
            goto fail;

        if( prev->size < expected_size ) // EOF
        {
            free_avpacket( pkt );
            return prev;
        }
        assert( prev->size == expected_size );

        AVPacket *next = get_samples( h, pivot, last_sample );
        if( !next )
        {
            free_avpacket( prev );
            goto fail;
        }

        memcpy( pkt->data, prev->data, prev->size );
        memcpy( pkt->data + prev->size, next->data, next->size );
        pkt->size = prev->size + next->size;

        free_avpacket( prev );
        free_avpacket( next );
    }
    else
    {
        int64_t lastreq = last_sample * h->info->samplesize;
        int64_t lastavail = fill_buffer_until( h, last_sample );
        if( lastavail < 0 )
            goto fail;

        intptr_t start  = ( first_sample * h->info->samplesize ) - h->bytepos;

        if( lastavail < lastreq )
        {
            pkt->size = lastavail - h->bytepos - start;
        }
        assert( start + pkt->size <= h->bufsize );
        memcpy( pkt->data, h->buffer + start, pkt->size );
    }

    return pkt;

fail:
    free_avpacket( pkt );
    return NULL;
}

static enum AudioResult close( hnd_t handle )
{
    assert( handle );
    lavf_source_t *h = handle;
    av_free( h->buffer );
    avcodec_close( h->ctx );
    av_close_input_file( h->lavf );
    free( h->info );
    free( h );

    return AUDIO_OK;
}

const audio_filter_t audio_lavf_source =
{
        .name        = "libavformat audio source",
        .description = "Demuxes and decodes audio files using libavformat + libavcodec",
        .help        = "Arguments: filename[:track]",
        .init        = init,
        .get_samples = get_samples,
        .free_packet = free_packet,
        .close       = close
};
