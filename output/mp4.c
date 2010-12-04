/*****************************************************************************
 * mp4.c: mp4 muxer using L-SMASH
 *****************************************************************************
 * Copyright (C) 2003-2010 x264 project
 *
 * Authors: Laurent Aimar <fenrir@via.ecp.fr>
 *          Loren Merritt <lorenm@u.washington.edu>
 *          Yusuke Nakamura <muken.the.vfrmaniac@gmail.com>
 *          Takashi Hirata <silverfilain@gmail.com>
 *          golgol7777 <golgol7777@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02111, USA.
 *
 * This program is also available under a commercial proprietary license.
 * For more information, contact us at licensing@x264.com.
 *****************************************************************************/

#include "common/common.h"
#include "output.h"
#include "mp4/isom.h"
#include "mp4/importer.h" /* FIXME: will be replaced with summary.h */

/*******************/

#define USE_LSMASH_IMPORTER 0

#if ( defined(HAVE_AUDIO) && HAVE_AUDIO ) || ( defined(USE_LSMASH_IMPORTER) && USE_LSMASH_IMPORTER )
#define HAVE_ANY_AUDIO 1
#else
#define HAVE_ANY_AUDIO 0
#endif

/*******************/

#define MP4_LOG_ERROR( ... )                x264_cli_log( "mp4", X264_LOG_ERROR, __VA_ARGS__ )
#define MP4_LOG_WARNING( ... )              x264_cli_log( "mp4", X264_LOG_WARNING, __VA_ARGS__ )
#define MP4_LOG_INFO( ... )                 x264_cli_log( "mp4", X264_LOG_INFO, __VA_ARGS__ )
//#define MP4_RETURN_IF_ERR( cond, ret, ... ) RETURN_IF_ERR( cond, "mp4", ret, __VA_ARGS__ )
#define MP4_FAIL_IF_ERR( cond, ... )        FAIL_IF_ERR( cond, "mp4", __VA_ARGS__ )

/* For close_file() */
#define MP4_LOG_IF_ERR( cond, ... )\
if( cond )\
{\
    MP4_LOG_ERROR( __VA_ARGS__ );\
}

/* For open_file() */
#define MP4_FAIL_IF_ERR_EX( cond, ... )\
if( cond )\
{\
    remove_mp4_hnd( p_mp4 );\
    MP4_LOG_ERROR( __VA_ARGS__ );\
    return -1;\
}

/*******************/

#if HAVE_ANY_AUDIO

#if HAVE_AUDIO
#include "audio/encoders.h"
#endif

typedef struct
{
    uint32_t i_track;
    uint32_t i_sample_entry;
    mp4sys_audio_summary_t *summary;
    uint64_t i_video_timescale;    /* For interleaving. */
    int i_numframe;
    enum isom_codec_code codec_type;
#if HAVE_AUDIO
    audio_info_t *info;
    hnd_t encoder;
    int has_sbr;
#else
    mp4sys_importer_t* p_importer;
#endif
} mp4_audio_hnd_t;
#endif /* #if HAVE_ANY_AUDIO */

typedef struct
{
    isom_root_t *p_root;
    char *psz_chapter;
    char *psz_language;
    int brand_3gpp;
    int brand_m4a;
    int brand_qt;
    uint32_t i_track;
    uint32_t i_sample_entry;
    uint64_t i_time_inc;
    int64_t i_start_offset;
    uint32_t i_sei_size;
    uint8_t *p_sei_buffer;
    int i_numframe;
    int64_t i_init_delta;
    int i_delay_frames;
    int b_dts_compress;
    int i_dts_compress_multiplier;
    int no_pasp;
    int b_use_recovery;
    int i_recovery_frame_cnt;
    int i_max_frame_num;
    uint64_t i_gop_head_cts;
    int b_no_remux;
#if HAVE_ANY_AUDIO
    mp4_audio_hnd_t *audio_hnd;
#endif
} mp4_hnd_t;

/*******************/

static void set_recovery_param( mp4_hnd_t *p_mp4, x264_param_t *p_param )
{
    p_mp4->b_use_recovery = p_param->i_open_gop || p_param->b_intra_refresh;
    if( !p_mp4->b_use_recovery )
        return;

    /* almost copied from x264_sps_init in encoder/set.c */
    int i_num_reorder_frames = p_param->i_bframe_pyramid ? 2 : p_param->i_bframe ? 1 : 0;
    int i_num_ref_frames = X264_MIN(X264_REF_MAX, X264_MAX4(p_param->i_frame_reference, 1 + i_num_reorder_frames,
                           p_param->i_bframe_pyramid ? 4 : 1, p_param->i_dpb_size));
    i_num_ref_frames -= p_param->i_bframe_pyramid == X264_B_PYRAMID_STRICT;
    if( p_param->i_keyint_max == 1 )
        i_num_ref_frames = 0;

    p_mp4->i_max_frame_num = i_num_ref_frames * (!!p_param->i_bframe_pyramid+1) + 1;
    if( p_param->b_intra_refresh )
    {
        p_mp4->i_recovery_frame_cnt = X264_MIN( ( p_param->i_width + 15 ) / 16 - 1, p_param->i_keyint_max ) + p_param->i_bframe - 1;
        p_mp4->i_max_frame_num = X264_MAX( p_mp4->i_max_frame_num, p_mp4->i_recovery_frame_cnt + 1 );
    }

    int i_log2_max_frame_num = 4;
    while( (1 << i_log2_max_frame_num) <= p_mp4->i_max_frame_num )
        i_log2_max_frame_num++;

    p_mp4->i_max_frame_num = 1 << i_log2_max_frame_num;
}

static void remove_mp4_hnd( hnd_t handle )
{
    mp4_hnd_t *p_mp4 = handle;
    if( !p_mp4 )
        return;
    if( p_mp4->p_sei_buffer )
    {
        free( p_mp4->p_sei_buffer );
        p_mp4->p_sei_buffer = NULL;
    }
    if( p_mp4->p_root )
    {
        isom_destroy_root( p_mp4->p_root );
        p_mp4->p_root = NULL;
    }
#if HAVE_ANY_AUDIO
    mp4_audio_hnd_t *p_audio = p_mp4->audio_hnd;
    if( !p_audio )
    {
        free( p_mp4 );
        return;
    }
#if HAVE_AUDIO
    if( p_audio->summary )
    {
        if( p_audio->summary->exdata )
        {
            free( p_audio->summary->exdata );
            p_audio->summary->exdata = NULL;
        }
        free( p_audio->summary );
        p_audio->summary = NULL;
    }
    if( p_audio->encoder )
    {
        x264_audio_encoder_close( p_audio->encoder );
        p_audio->encoder = NULL;
    }
#else
    if( p_audio->summary )
    {
        /* WARNING: You should not rely on this if you created summary in your own code instead of using importer of L-SMASH. */
        mp4sys_cleanup_audio_summary( p_audio->summary );
        p_audio->summary = NULL;
    }
    if( p_audio->p_importer )
    {
        mp4sys_importer_close( p_audio->p_importer );
        p_audio->p_importer = NULL;
    }
#endif
    free( p_audio );
    p_mp4->audio_hnd = NULL;
#endif /* #if HAVE_ANY_AUDIO */
    free( p_mp4 );
}

#if HAVE_AUDIO
static int audio_init( hnd_t handle, hnd_t filters, char *audio_enc, char *audio_parameters )
{
    if( !strcmp( audio_enc, "none" ) || !filters )
        return 0;

    // TODO: support other audio format
    hnd_t henc;

    if( !strcmp( audio_enc, "copy" ) )
        henc = x264_audio_copy_open( filters );
    else
    {
        char audio_params[MAX_ARGS];
        const char *used_enc;
        /* libopencore-amr does not have AMR-WB encoder yet, so we can't use it. */
        const audio_encoder_t *encoder = x264_select_audio_encoder( audio_enc, (char*[]){ "aac", "mp3", "ac3", "alac", "amrnb", "amrwb", NULL }, &used_enc );
        MP4_FAIL_IF_ERR( !encoder, "unable to select audio encoder.\n" );

        snprintf( audio_params, MAX_ARGS, "%s,codec=%s", audio_parameters, used_enc );
        henc = x264_audio_encoder_open( encoder, filters, audio_params );
    }

    MP4_FAIL_IF_ERR( !henc, "error opening audio encoder.\n" );
    mp4_hnd_t *p_mp4 = handle;
    mp4_audio_hnd_t *p_audio = p_mp4->audio_hnd = calloc( 1, sizeof( mp4_audio_hnd_t ) );
    audio_info_t *info = p_audio->info = x264_audio_encoder_info( henc );

    if( !strcmp( info->codec_name, "aac" ) )
    {
        p_audio->codec_type = ISOM_CODEC_TYPE_MP4A_AUDIO;
        audio_aac_info_t *aacinfo = info->opaque;
        if( aacinfo )
            p_audio->has_sbr = aacinfo->has_sbr;
        else
            p_audio->has_sbr = 0; // SBR presence isn't specified, so assume implicit signaling
        p_mp4->brand_m4a = 1;
    }
    else if( ( !strcmp( info->codec_name, "mp3" ) || !strcmp( info->codec_name, "mp2" ) || !strcmp( info->codec_name, "mp1" ) )
             && info->samplerate >= 16000 ) /* freq <16khz is MPEG-2.5. */
        p_audio->codec_type = ISOM_CODEC_TYPE_MP4A_AUDIO;
    else if( !strcmp( info->codec_name, "ac3" ) )
        p_audio->codec_type = ISOM_CODEC_TYPE_AC_3_AUDIO;
    else if( !strcmp( info->codec_name, "alac" ) )
    {
        p_audio->codec_type = ISOM_CODEC_TYPE_ALAC_AUDIO;
        p_mp4->brand_m4a = 1;
    }
    else if( !strcmp( info->codec_name, "amrnb" ) )
        p_audio->codec_type = ISOM_CODEC_TYPE_SAMR_AUDIO;
    else if( !strcmp( info->codec_name, "amrwb" ) )
        p_audio->codec_type = ISOM_CODEC_TYPE_SAWB_AUDIO;
    else
    {
        MP4_LOG_ERROR( "unsupported audio codec '%s'.\n", info->codec_name );
        goto error;
    }

    p_audio->encoder = henc;

    p_audio->summary = calloc( 1, sizeof(mp4sys_audio_summary_t) );
    MP4_FAIL_IF_ERR( !p_audio->summary,
                     "failed to allocate memory for summary information of audio.\n" );

    return 1;

error:
    x264_audio_encoder_close( henc );
    free( p_mp4->audio_hnd );
    p_mp4->audio_hnd = NULL;

    return -1;
}
#endif /* #if HAVE_AUDIO */

#if HAVE_ANY_AUDIO
static int write_audio_frames( mp4_hnd_t *p_mp4, double video_dts, int finish )
{
    mp4_audio_hnd_t *p_audio = p_mp4->audio_hnd;
    assert( p_audio );

#if HAVE_AUDIO
    audio_packet_t *frame;
#endif

    /* FIXME: This is just a sample implementation. */
    for(;;)
    {
        uint64_t audio_timestamp = (uint64_t)p_audio->i_numframe * p_audio->summary->samples_in_frame;
        /*
         * means while( audio_dts < video_dts )
         * FIXME: I wonder if there's any way more effective.
         */
        if( !finish && audio_timestamp / (double)p_audio->summary->frequency > video_dts )
            break;

        /* read a audio frame */
#if HAVE_AUDIO
        if( finish )
            frame = x264_audio_encoder_finish( p_audio->encoder );
        else if( !(frame = x264_audio_encode_frame( p_audio->encoder )) )
        {
            finish = 1;
            continue;
        }

        if( !frame )
            break;

        isom_sample_t *p_sample = isom_create_sample( frame->size );
        MP4_FAIL_IF_ERR( !p_sample,
                         "failed to create a audio sample data.\n" );
        memcpy( p_sample->data, frame->data, frame->size );
        x264_audio_free_frame( p_audio->encoder, frame );
#else
        /* FIXME: mp4sys_importer_get_access_unit() returns 1 if there're any changes in stream's properties.
           If you want to support them, you have to retrieve summary again, and make some operation accordingly. */
        isom_sample_t *p_sample = isom_create_sample( p_audio->summary->max_au_length );
        MP4_FAIL_IF_ERR( !p_sample,
                         "failed to create a audio sample data.\n" );
        MP4_FAIL_IF_ERR( mp4sys_importer_get_access_unit( p_audio->p_importer, 1, p_sample->data, &p_sample->length ),
                         "failed to retrieve frame data from importer.\n" );
        if( p_sample->length == 0 )
            break; /* end of stream */
#endif

        p_sample->dts = audio_timestamp;
        p_sample->cts = audio_timestamp;
        p_sample->prop.sync_point = 0; /* means, every sample can be random access point. */
        p_sample->index = p_audio->i_sample_entry;
        MP4_FAIL_IF_ERR( isom_write_sample( p_mp4->p_root, p_audio->i_track, p_sample ),
                         "failed to write a audio sample.\n" );

        p_audio->i_numframe++;
    }
    return 0;
}
#endif /* #if HAVE_ANY_AUDIO */

int remux_callback( void* param, uint64_t done, uint64_t total )
{
    int64_t elapsed = x264_mdate() - *(int64_t*)param;
    double byterate = done / ( elapsed / 1000000. );
    fprintf( stderr, "remux [%5.2lf%%], %"PRIu64"/%"PRIu64" KiB, %u KiB/s, ",
        done*100./total, done/1024, total/1024, (unsigned)byterate/1024 );
    if( done == total )
    {
        unsigned sec = (unsigned)( elapsed / 1000000 );
        fprintf( stderr, "total elapsed %u:%02u:%02u\n\n", sec/3600, (sec/60)%60, sec%60 );
    }
    else
    {
        unsigned eta = (unsigned)( (total - done) / byterate );
        fprintf( stderr, "eta %u:%02u:%02u\r", eta/3600, (eta/60)%60, eta%60 );
    }
    fflush( stderr ); // needed in windows
    return 0;
}

/*******************/

static int close_file( hnd_t handle, int64_t largest_pts, int64_t second_largest_pts )
{
    mp4_hnd_t *p_mp4 = handle;

    if( !p_mp4 )
        return 0;

    if( p_mp4->p_root )
    {
        /* Flush the rest of samples and add the last sample_delta. */
        uint32_t last_delta = largest_pts - second_largest_pts;
        MP4_LOG_IF_ERR( isom_flush_pooled_samples( p_mp4->p_root, p_mp4->i_track, (last_delta ? last_delta : 1) * p_mp4->i_time_inc ),
                        "failed to flush the rest of samples.\n" );

        /*
         * Declare the explicit time-line mapping.
         * A segment_duration is given by movie timescale, while a media_time that is the start time of this segment
         * is given by not the movie timescale but rather the media timescale.
         * The reason is that ISO media have two time-lines, presentation and media time-line,
         * and an edit maps the presentation time-line to the media time-line.
         * According to QuickTime file format specification and the actual playback in QuickTime Player,
         * if the Edit Box doesn't exist in the track, the ratio of the summation of sample durations and track's duration becomes
         * the track's media_rate so that the entire media can be used by the track.
         * So, we add Edit Box here to avoid this implicit media_rate could distort track's presentation timestamps slightly.
         * Note: Any demuxers should follow the Edit List Box if it exists.
         */
        uint32_t mvhd_timescale = isom_get_movie_timescale( p_mp4->p_root );
        uint32_t mdhd_timescale = isom_get_media_timescale( p_mp4->p_root, p_mp4->i_track );
        double actual_duration  = 0;
        if( mdhd_timescale != 0 ) /* avoid zero division */
        {
            actual_duration = (double)((largest_pts + last_delta) * p_mp4->i_time_inc) * mvhd_timescale / mdhd_timescale;
            MP4_LOG_IF_ERR( isom_create_explicit_timeline_map( p_mp4->p_root, p_mp4->i_track, actual_duration, p_mp4->i_start_offset * p_mp4->i_time_inc, ISOM_NORMAL_EDIT ),
                            "failed to set timeline map for video.\n" );
        }
        else
            MP4_LOG_ERROR( "mdhd timescale is broken.\n" );

        MP4_LOG_IF_ERR( isom_update_bitrate_info( p_mp4->p_root, p_mp4->i_track, p_mp4->i_sample_entry ),
                        "failed to update bitrate information for video.\n" );
#if HAVE_ANY_AUDIO
        mp4_audio_hnd_t *p_audio = p_mp4->audio_hnd;
        if( p_audio )
        {
            MP4_LOG_IF_ERR( ( write_audio_frames( p_mp4, actual_duration / mvhd_timescale, 0 ) ||
                              write_audio_frames( p_mp4, 0, 1 ) ),
                            "failed to flush audio frame(s).\n" );
#if HAVE_AUDIO
            MP4_LOG_IF_ERR( isom_flush_pooled_samples( p_mp4->p_root, p_audio->i_track, p_audio->info->last_delta ),
                            "failed to set last sample's duration for audio.\n" );
#else
            MP4_LOG_IF_ERR( isom_flush_pooled_samples( p_mp4->p_root, p_audio->i_track, p_audio->summary->samples_in_frame ),
                            "failed to set last sample's duration for audio.\n" );
#endif
            MP4_LOG_IF_ERR( isom_create_explicit_timeline_map( p_mp4->p_root, p_audio->i_track, 0, 0, ISOM_NORMAL_EDIT ),
                            "failed to set timeline map for audio.\n" );
            MP4_LOG_IF_ERR( isom_update_bitrate_info( p_mp4->p_root, p_audio->i_track, p_audio->i_sample_entry ),
                            "failed to update bitrate information for audio.\n" );
        }
#endif

        if( p_mp4->psz_chapter )
            MP4_LOG_IF_ERR( isom_set_tyrant_chapter( p_mp4->p_root, p_mp4->psz_chapter ), "failed to set chapter list.\n" );

        if( !p_mp4->b_no_remux )
        {
            int64_t start = x264_mdate();
            isom_adhoc_remux_t remux_info;
            remux_info.func = remux_callback;
            remux_info.buffer_size = 4*1024*1024; // 4MiB
            remux_info.param = &start;
            MP4_LOG_IF_ERR( isom_finish_movie( p_mp4->p_root, &remux_info ), "failed to finish movie.\n" );
        }
        else
            MP4_LOG_IF_ERR( isom_finish_movie( p_mp4->p_root, NULL ), "failed to finish movie.\n" );

        /* Write media data size here. */
        MP4_LOG_IF_ERR( isom_write_mdat_size( p_mp4->p_root ), "failed to write mdat size.\n" );
    }

    remove_mp4_hnd( p_mp4 ); /* including isom_destroy_root( p_mp4->p_root ); */

    return 0;
}

static int open_file( char *psz_filename, hnd_t *p_handle, cli_output_opt_t *opt, hnd_t audio_filters, char *audio_enc, char *audio_params )
{
    mp4_hnd_t *p_mp4;

    *p_handle = NULL;
    FILE *fh = fopen( psz_filename, "wb" );
    MP4_FAIL_IF_ERR( !fh, "cannot open output file `%s'.\n", psz_filename );
    if( !x264_is_regular_file( fh ) )
    {
        fclose( fh );
        MP4_FAIL_IF_ERR( -1, "MP4 output is incompatible with non-regular file `%s'\n", psz_filename );
    }
    fclose( fh );

    p_mp4 = malloc( sizeof(mp4_hnd_t) );
    MP4_FAIL_IF_ERR( !p_mp4, "failed to allocate memory for muxer information.\n" );
    memset( p_mp4, 0, sizeof(mp4_hnd_t) );

    p_mp4->b_dts_compress = opt->use_dts_compress;

    if( opt->chapter )
    {
        p_mp4->psz_chapter = opt->chapter;
        fh = fopen( p_mp4->psz_chapter, "rb" );
        MP4_FAIL_IF_ERR_EX( !fh, "can't open `%s'\n", p_mp4->psz_chapter );
        fclose( fh );
    }
    p_mp4->psz_language = opt->language;
    p_mp4->no_pasp = opt->no_sar;
    p_mp4->b_no_remux = opt->no_remux;

    p_mp4->p_root = isom_create_movie( psz_filename );
    MP4_FAIL_IF_ERR_EX( !p_mp4->p_root, "failed to create root.\n" );

#if HAVE_ANY_AUDIO
#if HAVE_AUDIO
    MP4_FAIL_IF_ERR_EX( audio_init( p_mp4, audio_filters, audio_enc, audio_params ) < 0, "unable to init audio output.\n" );
#else
    mp4_audio_hnd_t *p_audio = p_mp4->audio_hnd = (mp4_audio_hnd_t *)malloc( sizeof(mp4_audio_hnd_t) );
    MP4_FAIL_IF_ERR_EX( !p_audio, "failed to allocate memory for audio muxing information.\n" );
    memset( p_audio, 0, sizeof(mp4_audio_hnd_t) );
    p_audio->p_importer = mp4sys_importer_open( "x264_audio_test.adts", "auto" );
    if( !p_audio->p_importer )
    {
        free( p_audio );
        p_mp4->audio_hnd = NULL;
    }
#endif
    if( !p_mp4->audio_hnd )
        MP4_LOG_INFO( "audio muxing feature is disabled.\n" );
#endif

    char* ext = get_filename_extension( psz_filename );
    if( !strcmp( ext, "3gp" ) )
        p_mp4->brand_3gpp = 1;
    else if( !strcmp( ext, "3g2" ) )
        p_mp4->brand_3gpp = 2;

    *p_handle = p_mp4;

    return 0;
}

static int set_param( hnd_t handle, x264_param_t *p_param )
{
    mp4_hnd_t *p_mp4 = handle;
    uint64_t i_media_timescale;

    set_recovery_param( p_mp4, p_param );

    p_mp4->i_delay_frames = p_param->i_bframe ? (p_param->i_bframe_pyramid ? 2 : 1) : 0;
    p_mp4->i_dts_compress_multiplier = p_mp4->b_dts_compress * p_mp4->i_delay_frames + 1;

    i_media_timescale = p_param->i_timebase_den * p_mp4->i_dts_compress_multiplier;
    p_mp4->i_time_inc = p_param->i_timebase_num * p_mp4->i_dts_compress_multiplier;
    FAIL_IF_ERR( i_media_timescale > UINT32_MAX, "mp4", "MP4 media timescale %"PRIu64" exceeds maximum\n", i_media_timescale );

    /* Set brands. */
    uint32_t brands[9] = { ISOM_BRAND_TYPE_ISOM, ISOM_BRAND_TYPE_MP41, ISOM_BRAND_TYPE_MP42, 0, 0, 0, 0, 0, 0 };
    uint32_t minor_version = 0;
    uint32_t brand_count = 3;
    if( p_mp4->brand_3gpp == 1 )
        brands[brand_count++] = ISOM_BRAND_TYPE_3GP6;
    else if( p_mp4->brand_3gpp == 2 )
    {
        brands[brand_count++] = ISOM_BRAND_TYPE_3GP6;
        brands[brand_count++] = ISOM_BRAND_TYPE_3G2A;
        minor_version = 0x00010000;
    }
    if( p_mp4->brand_m4a )
        brands[brand_count++] = ISOM_BRAND_TYPE_M4A;
    if( p_mp4->b_use_recovery )
    {
        brands[brand_count++] = ISOM_BRAND_TYPE_AVC1;   /* sdtp/sgpd/sbgp */
        brands[brand_count++] = ISOM_BRAND_TYPE_ISO4;   /* cslg */
        brands[brand_count++] = ISOM_BRAND_TYPE_QT;     /* tapt/cslg/stps/sdtp */
        p_mp4->brand_qt = 1;
    }
    MP4_FAIL_IF_ERR( isom_set_brands( p_mp4->p_root, brands[2+p_mp4->brand_3gpp], minor_version, brands, brand_count ),
                     "failed to set brands / ftyp.\n" );

    /* Set max duration per chunk. */
    MP4_FAIL_IF_ERR( isom_set_max_chunk_duration( p_mp4->p_root, 0.5 ),
                     "failed to set max duration per chunk.\n" );

    /* Create a video track. */
    p_mp4->i_track = isom_create_track( p_mp4->p_root, ISOM_MEDIA_HANDLER_TYPE_VIDEO );
    MP4_FAIL_IF_ERR_EX( !p_mp4->i_track, "failed to create a video track.\n" );

    /* Set timescale. */
    MP4_FAIL_IF_ERR( isom_set_movie_timescale( p_mp4->p_root, 600 ),
                     "failed to set movie timescale.\n" );
    MP4_FAIL_IF_ERR( isom_set_media_timescale( p_mp4->p_root, p_mp4->i_track, i_media_timescale ),
                     "failed to set media timescale for video.\n" );

    /* Set handler name. */
    MP4_FAIL_IF_ERR( isom_set_media_handler_name( p_mp4->p_root, p_mp4->i_track, "X264 Video Media Handler" ),
                     "failed to set media hander name for video.\n" );
    if( p_mp4->brand_qt )
        MP4_FAIL_IF_ERR( isom_set_data_handler_name( p_mp4->p_root, p_mp4->i_track, "X264 URL Data Handler" ),
                         "failed to set data hander name for video.\n" );

    if( p_mp4->b_use_recovery )
        MP4_FAIL_IF_ERR( isom_create_grouping( p_mp4->p_root, p_mp4->i_track, ISOM_GROUP_TYPE_ROLL ),
                         "failed to create roll recovery grouping\n" );

    /* Add a sample entry. */
    /* FIXME: I think these sample_entry relative stuff should be more encapsulated, by using video_summary. */
    p_mp4->i_sample_entry = isom_add_sample_entry( p_mp4->p_root, p_mp4->i_track, ISOM_CODEC_TYPE_AVC1_VIDEO, NULL );
    MP4_FAIL_IF_ERR( !p_mp4->i_sample_entry,
                     "failed to add sample entry for video.\n" );
    MP4_FAIL_IF_ERR( isom_add_btrt( p_mp4->p_root, p_mp4->i_track, p_mp4->i_sample_entry ),
                     "failed to add btrt.\n" );
    MP4_FAIL_IF_ERR( isom_set_sample_resolution( p_mp4->p_root, p_mp4->i_track, p_mp4->i_sample_entry, (uint16_t)p_param->i_width, (uint16_t)p_param->i_height ),
                     "failed to set sample resolution.\n" );
    uint64_t dw = p_param->i_width << 16;
    uint64_t dh = p_param->i_height << 16;
    if( p_param->vui.i_sar_width && p_param->vui.i_sar_height )
    {
        double sar = (double)p_param->vui.i_sar_width / p_param->vui.i_sar_height;
        if( sar > 1.0 )
            dw *= sar;
        else
            dh /= sar;
        if( !p_mp4->no_pasp )
            MP4_FAIL_IF_ERR( isom_set_sample_aspect_ratio( p_mp4->p_root, p_mp4->i_track, p_mp4->i_sample_entry, p_param->vui.i_sar_width, p_param->vui.i_sar_height ),
                             "failed to set sample aspect ratio.\n" );
    }
    MP4_FAIL_IF_ERR( isom_set_track_presentation_size( p_mp4->p_root, p_mp4->i_track, dw, dh ),
                     "failed to set presentation size.\n" );
    if( p_mp4->brand_qt && !p_mp4->no_pasp )
        MP4_FAIL_IF_ERR( isom_set_track_aperture_modes( p_mp4->p_root, p_mp4->i_track, p_mp4->i_sample_entry ),
                         "failed to set track aperture mode.\n" );

    if( p_mp4->psz_language )
        MP4_FAIL_IF_ERR( isom_set_media_language( p_mp4->p_root, p_mp4->i_track, p_mp4->psz_language, 0 ),
                         "failed to set language for video.\n");

#if HAVE_ANY_AUDIO
    mp4_audio_hnd_t *p_audio = p_mp4->audio_hnd;
    if( p_audio )
    {
        /* Create a audio track. */
        p_audio->i_track = isom_create_track( p_mp4->p_root, ISOM_MEDIA_HANDLER_TYPE_AUDIO );
        MP4_FAIL_IF_ERR_EX( !p_audio->i_track, "failed to create a audio track.\n" );

#if HAVE_AUDIO
        p_audio->summary->stream_type      = MP4SYS_STREAM_TYPE_AudioStream;
        p_audio->summary->max_au_length    = ( 1 << 13 ) - 1;
        p_audio->summary->frequency        = p_audio->info->samplerate;
        p_audio->summary->channels         = p_audio->info->channels;
        p_audio->summary->bit_depth        = 16;
        p_audio->summary->samples_in_frame = p_audio->info->framelen;
        switch( p_audio->codec_type )
        {
            case ISOM_CODEC_TYPE_MP4A_AUDIO :
                p_audio->summary->object_type_indication = MP4SYS_OBJECT_TYPE_Audio_ISO_14496_3;
                if( !strcmp( p_audio->info->codec_name, "mp3" ) || !strcmp( p_audio->info->codec_name, "mp2" ) || !strcmp( p_audio->info->codec_name, "mp1" ) )
                {
                    if( p_audio->info->samplerate >= 32000 )
                        p_audio->summary->object_type_indication = MP4SYS_OBJECT_TYPE_Audio_ISO_11172_3; /* Legacy Interface */
                    else
                        p_audio->summary->object_type_indication = MP4SYS_OBJECT_TYPE_Audio_ISO_13818_3; /* Legacy Interface */
                }
                p_audio->summary->aot                    = MP4A_AUDIO_OBJECT_TYPE_AAC_LC;
                p_audio->summary->sbr_mode               = p_audio->has_sbr ? MP4A_AAC_SBR_BACKWARD_COMPATIBLE : MP4A_AAC_SBR_NOT_SPECIFIED;
                p_audio->summary->exdata_length          = p_audio->info->extradata_size;
                p_audio->summary->exdata                 = malloc( p_audio->info->extradata_size );
                memcpy( p_audio->summary->exdata, p_audio->info->extradata, p_audio->info->extradata_size );
                break;
            case ISOM_CODEC_TYPE_AC_3_AUDIO :
                p_audio->summary->object_type_indication = MP4SYS_OBJECT_TYPE_AC_3_AUDIO;
                MP4_FAIL_IF_ERR( mp4sys_create_dac3_from_syncframe( p_audio->summary, p_audio->info->extradata, p_audio->info->extradata_size ),
                                 "failed to create AC-3 specific info.\n" );
                break;
            case ISOM_CODEC_TYPE_SAMR_AUDIO :
                p_audio->summary->object_type_indication = MP4SYS_OBJECT_TYPE_PRIV_SAMR_AUDIO;
                MP4_FAIL_IF_ERR( mp4sys_amr_create_damr( p_audio->summary ),
                                 "failed to create AMR specific info.\n" );
                break;
            case ISOM_CODEC_TYPE_SAWB_AUDIO :
                p_audio->summary->object_type_indication = MP4SYS_OBJECT_TYPE_PRIV_SAWB_AUDIO;
                MP4_FAIL_IF_ERR( mp4sys_amr_create_damr( p_audio->summary ),
                                 "failed to create AMR specific info.\n" );
                break;
            default :
                p_audio->summary->object_type_indication = MP4SYS_OBJECT_TYPE_NONE;
                p_audio->summary->exdata_length          = p_audio->info->extradata_size;
                p_audio->summary->exdata                 = malloc( p_audio->info->extradata_size );
                memcpy( p_audio->summary->exdata, p_audio->info->extradata, p_audio->info->extradata_size );
                break;
        }
#else
        /*
         * NOTE: Retrieve audio summary, which will be used for isom_add_sample_entry() as audio parameters.
         * Currently, our ADTS importer never recognize SBR (HE-AAC).
         * Thus, in this sample, if the source contains SBR, it'll be coded as implicit signaling for SBR.
         * If you want to use explicit signaling of SBR, change the sbr_mode in summary and
         * call mp4sys_setup_AudioSpecificConfig() to reconstruct ASC within the summary.
         */
        /*
         * WARNING: If you wish to allocate summary in your code, you have to allocate ASC too,
         * and never use mp4sys_cleanup_audio_summary(), when L-SMASH is not compiled integrated with your code.
         * Because malloc() and free() have to be used as pair from EXACTLY SAME standard C library.
         * Otherwise you may cause bugs which you hardly call to mind.
         */
        p_audio->summary = mp4sys_duplicate_audio_summary( p_audio->p_importer, 1 );
        switch( p_audio->summary->object_type_indication )
        {
        case MP4SYS_OBJECT_TYPE_Audio_ISO_14496_3:
        case MP4SYS_OBJECT_TYPE_Audio_ISO_13818_3: /* Legacy Interface */
        case MP4SYS_OBJECT_TYPE_Audio_ISO_11172_3: /* Legacy Interface */
            p_audio->codec_type = ISOM_CODEC_TYPE_MP4A_AUDIO; break;
        case MP4SYS_OBJECT_TYPE_PRIV_SAMR_AUDIO:
            p_audio->codec_type = ISOM_CODEC_TYPE_SAMR_AUDIO; break;
        case MP4SYS_OBJECT_TYPE_PRIV_SAWB_AUDIO:
            p_audio->codec_type = ISOM_CODEC_TYPE_SAWB_AUDIO; break;
        default:
            MP4_FAIL_IF_ERR( 1, "Unknown object_type_indication.\n" );
        }
#endif /* #if HAVE_AUDIO #else */
        p_audio->i_video_timescale = i_media_timescale;
        MP4_FAIL_IF_ERR( isom_set_media_timescale( p_mp4->p_root, p_audio->i_track, p_audio->summary->frequency ),
                         "failed to set media timescale for audio.\n");
        MP4_FAIL_IF_ERR( isom_set_media_handler_name( p_mp4->p_root, p_audio->i_track, "X264 Sound Media Handler" ),
                         "failed to set media handler name for audio.\n" );
        if( p_mp4->brand_qt )
            MP4_FAIL_IF_ERR( isom_set_data_handler_name( p_mp4->p_root, p_audio->i_track, "X264 URL Data Handler" ),
                             "failed to set data hander name for audio.\n" );
        p_audio->i_sample_entry = isom_add_sample_entry( p_mp4->p_root, p_audio->i_track, p_audio->codec_type, p_audio->summary );
        MP4_FAIL_IF_ERR( !p_audio->i_sample_entry,
                         "failed to add sample_entry for audio.\n" );
        /* MP4AudioSampleEntry does not have btrt */
//        MP4_FAIL_IF_ERR( isom_add_btrt( p_mp4->p_root, p_audio->i_track, p_audio->i_sample_entry ),
//                         "failed to add btrt for audio.\n" );
        if( p_mp4->psz_language )
            MP4_FAIL_IF_ERR( isom_set_media_language( p_mp4->p_root, p_audio->i_track, p_mp4->psz_language, 0 ),
                             "failed to set language for audio track.\n" );
    }
#endif /* #if HAVE_ANY_AUDIO */

    return 0;
}

static int write_headers( hnd_t handle, x264_nal_t *p_nal )
{
    mp4_hnd_t *p_mp4 = handle;

    uint32_t sps_size = p_nal[0].i_payload - 4;
    uint32_t pps_size = p_nal[1].i_payload - 4;
    uint32_t sei_size = p_nal[2].i_payload;

    uint8_t *sps = p_nal[0].p_payload + 4;
    uint8_t *pps = p_nal[1].p_payload + 4;
    uint8_t *sei = p_nal[2].p_payload;

    MP4_FAIL_IF_ERR( isom_set_avc_config( p_mp4->p_root, p_mp4->i_track, p_mp4->i_sample_entry, 1, sps[1], sps[2], sps[3], 3, 1, BIT_DEPTH-8, BIT_DEPTH-8 ),
                     "failed to set avc config.\n" );
    /* SPS */
    MP4_FAIL_IF_ERR( isom_add_sps_entry( p_mp4->p_root, p_mp4->i_track, p_mp4->i_sample_entry, sps, sps_size ),
                     "failed to add sps.\n" );
    /* PPS */
    MP4_FAIL_IF_ERR( isom_add_pps_entry( p_mp4->p_root, p_mp4->i_track, p_mp4->i_sample_entry, pps, pps_size ),
                     "failed to add pps.\n" );
    /* SEI */
    p_mp4->p_sei_buffer = malloc( sei_size );
    MP4_FAIL_IF_ERR( !p_mp4->p_sei_buffer,
                     "failed to allocate sei transition buffer.\n" );
    memcpy( p_mp4->p_sei_buffer, sei, sei_size );
    p_mp4->i_sei_size = sei_size;

    /* Write ftyp. */
    MP4_FAIL_IF_ERR( isom_write_ftyp( p_mp4->p_root ),
                     "failed to write brands / ftyp.\n" );

    /* Write mdat header. */
    MP4_FAIL_IF_ERR( isom_add_mdat( p_mp4->p_root ), "failed to add mdat.\n" );

    return sei_size + sps_size + pps_size;
}

static int write_frame( hnd_t handle, uint8_t *p_nalu, int i_size, x264_picture_t *p_picture )
{
    mp4_hnd_t *p_mp4 = handle;
    uint64_t dts, cts;

    if( !p_mp4->i_numframe )
    {
        p_mp4->i_start_offset = p_picture->i_dts * -1;
        if( p_mp4->psz_chapter && p_mp4->brand_qt )
            MP4_FAIL_IF_ERR( isom_create_reference_chapter_track( p_mp4->p_root, p_mp4->i_track, p_mp4->psz_chapter ),
                             "failed to create reference chapter track.\n" );
    }

    isom_sample_t *p_sample = isom_create_sample( i_size + p_mp4->i_sei_size );
    MP4_FAIL_IF_ERR( !p_sample,
                     "failed to create a video sample data.\n" );

    if( p_mp4->p_sei_buffer )
    {
        memcpy( p_sample->data, p_mp4->p_sei_buffer, p_mp4->i_sei_size );
        free( p_mp4->p_sei_buffer );
        p_mp4->p_sei_buffer = NULL;
    }

    memcpy( p_sample->data + p_mp4->i_sei_size, p_nalu, i_size );
    p_mp4->i_sei_size = 0;

    if( p_mp4->b_dts_compress )
    {
        if( p_mp4->i_numframe == 1 )
            p_mp4->i_init_delta = (p_picture->i_dts + p_mp4->i_start_offset) * p_mp4->i_time_inc;
        dts = p_mp4->i_numframe > p_mp4->i_delay_frames
            ? p_picture->i_dts * p_mp4->i_time_inc
            : p_mp4->i_numframe * (p_mp4->i_init_delta / p_mp4->i_dts_compress_multiplier);
        cts = p_picture->i_pts * p_mp4->i_time_inc;
    }
    else
    {
        dts = (p_picture->i_dts + p_mp4->i_start_offset) * p_mp4->i_time_inc;
        cts = (p_picture->i_pts + p_mp4->i_start_offset) * p_mp4->i_time_inc;
    }

    p_sample->dts = dts;
    p_sample->cts = cts;
    p_sample->index = p_mp4->i_sample_entry;
    p_sample->prop.sync_point = p_picture->i_type == X264_TYPE_IDR;
    if( p_mp4->b_use_recovery )
    {
        p_sample->prop.partial_sync = (p_picture->i_type == X264_TYPE_I) && p_picture->b_keyframe && (p_mp4->i_recovery_frame_cnt == 0);
        p_sample->prop.independent = IS_X264_TYPE_I( p_picture->i_type ) ? ISOM_SAMPLE_IS_INDEPENDENT : ISOM_SAMPLE_IS_NOT_INDEPENDENT;
        p_sample->prop.disposable = p_picture->i_type == X264_TYPE_B ? ISOM_SAMPLE_IS_DISPOSABLE : ISOM_SAMPLE_IS_NOT_DISPOSABLE;
        p_sample->prop.redundant = ISOM_SAMPLE_HAS_NO_REDUNDANCY;
        p_sample->prop.leading = !IS_X264_TYPE_B( p_picture->i_type ) || p_sample->cts >= p_mp4->i_gop_head_cts ? ISOM_SAMPLE_IS_NOT_LEADING : ISOM_SAMPLE_IS_UNDECODABLE_LEADING;
        p_sample->prop.recovery.start_point = p_picture->b_keyframe && p_picture->i_type != X264_TYPE_IDR;   /* A picture with Recovery Point SEI */
        p_sample->prop.recovery.identifier = p_picture->i_frame_num % p_mp4->i_max_frame_num;
        if( p_sample->prop.recovery.start_point )
            p_sample->prop.recovery.complete = (p_sample->prop.recovery.identifier + p_mp4->i_recovery_frame_cnt) % p_mp4->i_max_frame_num;
        if( p_sample->prop.sync_point || p_sample->prop.partial_sync )
            p_mp4->i_gop_head_cts = p_sample->cts;
    }

    x264_cli_log( "mp4", X264_LOG_DEBUG, "coded: %d, frame_num: %d, key: %s, type: %s, independ: %s, dispose: %s, lead: %s\n",
                  p_mp4->i_numframe, p_picture->i_frame_num, p_picture->b_keyframe ? "yes" : "no",
                  p_picture->i_type == X264_TYPE_P ? "P" : p_picture->i_type == X264_TYPE_B ? "b" :
                  p_picture->i_type == X264_TYPE_BREF ? "B" : p_picture->i_type == X264_TYPE_IDR ? "I" :
                  p_picture->i_type == X264_TYPE_I ? "i" : p_picture->i_type == X264_TYPE_KEYFRAME ? "K" : "N",
                  p_sample->prop.independent == ISOM_SAMPLE_IS_INDEPENDENT ? "yes" : "no",
                  p_sample->prop.disposable == ISOM_SAMPLE_IS_DISPOSABLE ? "yes" : "no",
                  p_sample->prop.leading == ISOM_SAMPLE_IS_UNDECODABLE_LEADING || p_sample->prop.leading == ISOM_SAMPLE_IS_DECODABLE_LEADING ? "yes" : "no" );

    /* Write data per sample. */
    MP4_FAIL_IF_ERR( isom_write_sample( p_mp4->p_root, p_mp4->i_track, p_sample ),
                     "failed to write a video frame.\n" );

    p_mp4->i_numframe++;

#if HAVE_ANY_AUDIO
    mp4_audio_hnd_t *p_audio = p_mp4->audio_hnd;
    if( p_audio )
    {
        MP4_FAIL_IF_ERR( write_audio_frames( p_mp4, p_sample->dts / (double)p_audio->i_video_timescale, 0 ),
                         "failed to write audio frame(s).\n" );
    }
#endif

    return i_size;
}

const cli_output_t mp4_output = { open_file, set_param, write_headers, write_frame, close_file };
