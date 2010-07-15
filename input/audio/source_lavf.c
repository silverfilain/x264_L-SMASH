#include "filters/audio/internal.h"
#undef DECLARE_ALIGNED
#include "libavformat/avformat.h"
#include "libavcodec/avcodec.h"
#include <assert.h>
#include <stdio.h>
#include <inttypes.h>

typedef struct lavf_source_t
{
    AUDIO_FILTER_COMMON
    AVFormatContext *lavf;
    AVCodecContext *ctx;
    AVCodec *codec;

    int samplefmt;
    unsigned track;
    uint8_t *buffer;
    intptr_t bufsize;
    intptr_t surplus;
    intptr_t len;
    uint64_t bytepos;

    AVPacket *pkt;
} lavf_source_t;

#define DEFAULT_BUFSIZE AVCODEC_MAX_AUDIO_FRAME_SIZE * 2

static int buffer_next_frame( lavf_source_t *h );

const audio_filter_t audio_source_lavf;

static int init( hnd_t *handle, const char *opt_str )
{
    assert( opt_str );
    assert( !(*handle) ); // This must be the first filter
    char **opts = x264_split_options( opt_str, (const char*[]){ "filename", "track", NULL } );

    if( !opts )
        return -1;

    char *filename = x264_get_option( "filename", opts );
    char *trackstr = x264_otos( x264_get_option( "track", opts ), "any" );

    assert( filename );

    int track;
    if ( !strcmp( trackstr, "any" ) )
        track = TRACK_ANY;
    else
        track = x264_otoi( trackstr, TRACK_NONE );

    if( track == TRACK_NONE )
    {
        x264_cli_log( "lavfsource", X264_LOG_ERROR, "no valid track requested ('any', 0 or a positive integer)\n" );
        goto fail2;
    }

    INIT_FILTER_STRUCT( audio_source_lavf, lavf_source_t );

    av_register_all();
    if( !strcmp( filename, "-" ) )
        filename = "pipe:";

    if( av_open_input_file( &h->lavf, filename, NULL, 0, NULL ) )
    {
        AF_LOG_ERR( h, "could not open audio file\n" );
        goto fail;
    }

    if( av_find_stream_info( h->lavf ) < 0 )
    {
        AF_LOG_ERR( h, "could not find stream info\n" );
        goto fail;
    }

    unsigned tid = TRACK_NONE;
    if( track >= 0 )
    {
        if( track < h->lavf->nb_streams &&
            h->lavf->streams[track]->codec->codec_type == CODEC_TYPE_AUDIO )
            tid = track;
        else
            AF_LOG_ERR( h, "requested track %d is unavailable "
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
            AF_LOG_ERR( h, "could not find any audio track\n" );
    }

    if( tid == TRACK_NONE )
        goto fail;

    h->track = tid;

    h->ctx = h->lavf->streams[tid]->codec;
    h->codec = avcodec_find_decoder( h->ctx->codec_id );
    if( avcodec_open( h->ctx, h->codec ) )
        goto codecfail;

    h->samplefmt  = h->ctx->sample_fmt;
    h->info = (audio_info_t)
    {
        .samplerate     = h->ctx->sample_rate,
        .channels       = h->ctx->channels,
        .chanlayout     = h->ctx->channel_layout,
        .framelen       = h->ctx->frame_size,
        .framesize      = h->ctx->frame_size * sizeof( float ),
        .chansize       = av_get_bits_per_sample_format( h->samplefmt ) / 8,
        .samplesize     = av_get_bits_per_sample_format( h->samplefmt ) * h->ctx->channels / 8,
        .time_base_num  = h->ctx->time_base.num,
        .time_base_den  = h->ctx->time_base.den,
        .extradata      = h->ctx->extradata,
        .extradata_size = h->ctx->extradata_size
    };

    h->bufsize = DEFAULT_BUFSIZE;
    h->surplus = h->info.framesize * 3 / 2;
    assert( h->bufsize > h->surplus * 2 );
    h->buffer  = av_malloc( h->bufsize );

    if( !buffer_next_frame( h ) )
        goto codecfail;

    x264_free_string_array( opts );
    return 0;

codecfail:
    AF_LOG_ERR( h, "error opening the %s decoder for track %d\n", h->codec->name, h->track );
fail:
    if( h->lavf )
        av_close_input_file( h->lavf );
    if( h )
        free( h );
    *handle = NULL;
fail2:
    x264_free_string_array( opts );
    return -1;
}

static inline void free_avpacket( AVPacket *pkt )
{
    av_free_packet( pkt );
    free( pkt );
}

static void free_packet( hnd_t handle, audio_packet_t *pkt )
{
    pkt->owner = NULL;
    af_free_packet( pkt );
}

static struct AVPacket *next_packet( lavf_source_t *h )
{
    AVPacket *pkt = calloc( 1, sizeof( AVPacket ) );

    int ret;
    do
    {
        if( pkt->data )
            av_free_packet( pkt );
        if( (ret = av_read_frame( h->lavf, pkt )) )
        {
            if( ret != AVERROR_EOF )
                AF_LOG_ERR( h, "read error: %s\n", strerror( -ret ) );
            else
                AF_LOG( h, X264_LOG_INFO, "end of file reached\n" );
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
    static uint8_t desync_warn = 0;

    int len = 0, datalen = 0;

    while( h->pkt && pkt_temp.size > 0 )
    {
        datalen = buflen;
        len = avcodec_decode_audio3( h->ctx, (int16_t*) buf, &datalen, &pkt_temp );

        if( len < 0 ) {
            // Broken frame, drop
            if( !desync_warn++ ) // repeat the warning every 256 errors
                AF_LOG_WARN( h, "Decoding errors may cause audio desync\n" );
            pkt_temp.size = 0;
            break;
        }

        pkt_temp.data += len;
        pkt_temp.size -= len;

        if( datalen < 0 )
            continue;

        return datalen;
    }

    free_avpacket( h->pkt );
    h->pkt = next_packet( h );

    if( !h->pkt )
        return -1;

    pkt_temp.data = h->pkt->data;
    pkt_temp.size = h->pkt->size;

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
        h->len     -= dec->size;
        h->bytepos += dec->size;
    }
    memcpy( h->buffer + h->len, dec->data, dec->size );
    h->len += dec->size;

    free_avpacket( dec );

    return 1;
}

static inline int not_in_cache( lavf_source_t *h, int64_t sample )
{
    int64_t samplebyte = sample * h->info.samplesize;
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
        AF_LOG_ERR( h, "backwards seeking not supported yet "
                       "(requested sample %"PRIu64", first available is %"PRIu64")\n",
                       lastsample, h->bytepos / h->info.samplesize );
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


static struct audio_packet_t *get_samples( hnd_t handle, int64_t first_sample, int64_t last_sample )
{
    lavf_source_t *h = handle;
    assert( first_sample >= 0 && last_sample > first_sample );

    if( fill_buffer_until( h, first_sample ) < 0 )
        return NULL;

    audio_packet_t *pkt = calloc( 1, sizeof( audio_packet_t ) );
    pkt->channels       = h->info.channels;
    pkt->samplecount    = last_sample - first_sample;
    pkt->size           = pkt->samplecount * h->info.samplesize;

    if( pkt->size + h->surplus > h->bufsize )
    {
        int64_t pivot = first_sample + ( h->bufsize - h->surplus * 2 ) / h->info.samplesize;
        int64_t expected_size = ( pivot - first_sample ) * h->info.samplesize;

        audio_packet_t *prev = get_samples( h, first_sample, pivot );
        if( !prev )
            goto fail;

        if( prev->size < expected_size ) // EOF
        {
            af_free_packet( pkt );
            prev->flags |= AUDIO_FLAG_EOF;
            return prev;
        }
        assert( prev->size == expected_size );

        audio_packet_t *next = get_samples( h, pivot, last_sample );
        if( !next )
        {
            af_free_packet( prev );
            goto fail;
        }

        pkt->data = af_dup_buffer( prev->data, prev->channels, prev->samplecount );
        af_cat_buffer( pkt->data, pkt->samplecount, next->data, next->samplecount, pkt->channels );

        pkt->samplecount = prev->samplecount + next->samplecount;
        pkt->size        = prev->size + next->size;

        af_free_packet( prev );
        af_free_packet( next );
    }
    else
    {
        int64_t lastreq   = last_sample * h->info.samplesize;
        int64_t lastavail = fill_buffer_until( h, last_sample );
        if( lastavail < 0 )
            goto fail;

        intptr_t start = ( first_sample * h->info.samplesize ) - h->bytepos;

        if( lastavail < lastreq )
        {
            pkt->size        = lastavail - h->bytepos - start;
            pkt->samplecount = pkt->size / h->info.samplesize;
            pkt->flags       = AUDIO_FLAG_EOF;
        }
        assert( start + pkt->size <= h->bufsize );
        pkt->data = af_deinterleave2( h->buffer + start, h->samplefmt, pkt->channels, pkt->samplecount );
    }

    return pkt;

fail:
    af_free_packet( pkt );
    return NULL;
}

static void lavfsource_close( hnd_t handle )
{
    assert( handle );
    lavf_source_t *h = handle;
    av_free( h->buffer );
    free_avpacket( h->pkt );
    avcodec_close( h->ctx );
    av_close_input_file( h->lavf );
    free( h );
}

const audio_filter_t audio_source_lavf =
{
        .name        = "lavfsource",
        .description = "Demuxes and decodes audio files using libavformat + libavcodec",
        .help        = "Arguments: filename[:track]",
        .init        = init,
        .get_samples = get_samples,
        .free_packet = free_packet,
        .close       = lavfsource_close
};
