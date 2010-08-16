#include "filters/audio/internal.h"
#include <assert.h>
#include <stdio.h>
#include <inttypes.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#undef WIN32_LEAN_AND_MEAN

#define AVSC_NO_DECLSPEC
#undef EXTERN_C
#include "extras/avisynth_c.h"
#define AVSC_DECLARE_FUNC(name) name##_func name

#define AVS_INTERFACE_25 2
#define DEFAULT_BUFSIZE 192000 // 1 second of 48khz 32bit audio
                               // same as AVCODEC_MAX_AUDIO_FRAME_SIZE

#define LOAD_AVS_FUNC(name, continue_on_fail)\
{\
    h->func.name = (void*)GetProcAddress( h->library, #name );\
    if( !continue_on_fail && !h->func.name )\
        goto fail;\
}

typedef struct avs_source_t
{
    AUDIO_FILTER_COMMON

    AVS_Clip *clip;
    AVS_ScriptEnvironment *env;
    HMODULE library;
    enum SampleFmt sample_fmt;

    int64_t num_samples;
    int eof;
    uint8_t *buffer;
    intptr_t bufsize;

    struct
    {
        AVSC_DECLARE_FUNC( avs_clip_get_error );
        AVSC_DECLARE_FUNC( avs_create_script_environment );
        AVSC_DECLARE_FUNC( avs_delete_script_environment );
        AVSC_DECLARE_FUNC( avs_get_audio );
        AVSC_DECLARE_FUNC( avs_get_video_info );
        AVSC_DECLARE_FUNC( avs_function_exists );
        AVSC_DECLARE_FUNC( avs_invoke );
        AVSC_DECLARE_FUNC( avs_release_clip );
        AVSC_DECLARE_FUNC( avs_release_value );
        AVSC_DECLARE_FUNC( avs_take_clip );
    } func;
} avs_source_t;

const audio_filter_t audio_filter_avs;

static int x264_audio_avs_load_library( avs_source_t *h )
{
    h->library = LoadLibrary( "avisynth" );
    if( !h->library )
        return -1;
    LOAD_AVS_FUNC( avs_clip_get_error, 0 );
    LOAD_AVS_FUNC( avs_create_script_environment, 0 );
    LOAD_AVS_FUNC( avs_delete_script_environment, 1 );
    LOAD_AVS_FUNC( avs_get_audio, 0 );
    LOAD_AVS_FUNC( avs_get_video_info, 0 );
    LOAD_AVS_FUNC( avs_function_exists, 0 );
    LOAD_AVS_FUNC( avs_invoke, 0 );
    LOAD_AVS_FUNC( avs_release_clip, 0 );
    LOAD_AVS_FUNC( avs_release_value, 0 );
    LOAD_AVS_FUNC( avs_take_clip, 0 );
    return 0;
fail:
    FreeLibrary( h->library );
    return -1;
}

static int init( hnd_t *handle, const char *opt_str )
{
    assert( opt_str );
    assert( !(*handle) ); // This must be the first filter
    char **opts = x264_split_options( opt_str, (const char*[]){ "filename", "track", NULL } );

    if( !opts )
        return -1;

    char *filename = x264_get_option( "filename", opts );
    char *filename_ext = get_filename_extension( filename );

    if( !filename )
    {
        x264_cli_log( "avs", X264_LOG_ERROR, "no filename given" );
        goto fail2;
    }
    if( !strcmp( filename, "-" ) || ( filename_ext && strcmp( filename_ext, "avs" ) ) )
    {
        x264_cli_log( "avs", X264_LOG_ERROR, "AVS audio input is not available for non-script file" );
        goto fail2;
    }

    INIT_FILTER_STRUCT( audio_filter_avs, avs_source_t );

    if( x264_audio_avs_load_library( h ) )
    {
        AF_LOG_ERR( h, "failed to load avisynth\n" );
        goto error;
    }

    h->env = h->func.avs_create_script_environment( AVS_INTERFACE_25 );
    if( !h->env )
    {
        AF_LOG_ERR( h, "failed to initiate avisynth\n" );
        goto error;
    }

    AVS_Value arg = avs_new_value_string( filename );
    AVS_Value res;

    res = h->func.avs_invoke( h->env, "Import", arg, NULL );
    if( avs_is_error( res ) )
    {
        AF_LOG_ERR( h, "failed to initiate avisynth\n" );
        goto error;
    }

    h->clip = h->func.avs_take_clip( res, h->env );
    if( !avs_is_clip( res ) )
    {
        AF_LOG_ERR( h, "no valid clip is found\n" );
        goto error;
    }

    const AVS_VideoInfo *vi = h->func.avs_get_video_info( h->clip );
    if( !avs_has_audio( vi ) )
    {
        AF_LOG_ERR( h, "no valid audio track is found\n" );
        goto error;
    }

    h->func.avs_release_value( res );

    h->info.samplerate     = avs_samples_per_second( vi );
    h->info.channels       = avs_audio_channels( vi );
    h->info.framelen       = 1;
    h->info.chansize       = avs_bytes_per_channel_sample( vi );
    h->info.samplesize     = h->info.chansize * h->info.channels;
    h->info.framesize      = h->info.samplesize;
    h->info.timebase       = (timebase_t){ 1, h->info.samplerate };

    h->num_samples = vi->num_audio_samples;
    h->bufsize = DEFAULT_BUFSIZE;
    h->buffer  = malloc( h->bufsize );

    switch( avs_sample_type( vi ) )
    {
      case AVS_SAMPLE_INT16:
        h->sample_fmt = SMPFMT_S16;
        break;
      case AVS_SAMPLE_INT32:
        h->sample_fmt = SMPFMT_S32;
        break;
      case AVS_SAMPLE_FLOAT:
        h->sample_fmt = SMPFMT_FLT;
        break;
      case AVS_SAMPLE_INT24:
      case AVS_SAMPLE_INT8:
      default:
        h->sample_fmt = SMPFMT_NONE;
        break;
    }

    if( h->sample_fmt == SMPFMT_NONE )
    {
        AF_LOG_ERR( h, "unsuppported audio sample format\n" );
        goto error;
    }

    x264_free_string_array( opts );
    return 0;

error:
    AF_LOG_ERR( h, "error opening audio input script\n" );
fail:
    if( h )
        free( h );
    *handle = NULL;
fail2:
    x264_free_string_array( opts );
    return -1;
}

static void free_packet( hnd_t handle, audio_packet_t *pkt )
{
    pkt->owner = NULL;
    x264_af_free_packet( pkt );
}

static struct audio_packet_t *get_samples( hnd_t handle, int64_t first_sample, int64_t last_sample )
{
    avs_source_t *h = handle;
    assert( first_sample >= 0 && last_sample > first_sample );
    int64_t nsamples = last_sample - first_sample;

    if( h->eof )
        return NULL;

    if( h->num_samples <= last_sample )
    {
        nsamples = h->num_samples - first_sample;
        h->eof = 1;
    }

    audio_packet_t *pkt = calloc( 1, sizeof( audio_packet_t ) );
    pkt->info           = h->info;
    pkt->dts            = first_sample;
    pkt->channels       = h->info.channels;
    pkt->samplecount    = nsamples;
    pkt->size           = pkt->samplecount * h->info.samplesize;

    if( h->func.avs_get_audio( h->clip, h->buffer, first_sample, nsamples ) )
        goto fail;

    pkt->samples = x264_af_deinterleave2( h->buffer, h->sample_fmt, pkt->channels, pkt->samplecount );

    if( h->eof )
        pkt->flags |= AUDIO_FLAG_EOF;

    return pkt;

fail:
    x264_af_free_packet( pkt );
    return NULL;
}

static void avs_close( hnd_t handle )
{
    assert( handle );
    avs_source_t *h = handle;
    h->func.avs_release_clip( h->clip );
    if( h->func.avs_delete_script_environment )
        h->func.avs_delete_script_environment( h->env );
    FreeLibrary( h->library );
    free( h->buffer );
    free( h );
}

const audio_filter_t audio_filter_avs =
{
        .name        = "avs",
        .description = "Retrive PCM samples from the first audio track of an AVISynth script",
        .help        = "Arguments: filename",
        .init        = init,
        .get_samples = get_samples,
        .free_packet = free_packet,
        .close       = avs_close
};
