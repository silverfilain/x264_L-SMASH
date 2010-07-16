#ifndef FILTERS_AUDIO_AUDIO_FILTERS_H_
#define FILTERS_AUDIO_AUDIO_FILTERS_H_

#include <stdint.h>
#include "x264cli.h"
#include "filters/filters.h"

// Ripped from ffmpeg's avcodec.h
#ifndef CH_FRONT_LEFT
#define CH_FRONT_LEFT             0x00000001
#define CH_FRONT_RIGHT            0x00000002
#define CH_FRONT_CENTER           0x00000004
#define CH_LOW_FREQUENCY          0x00000008
#define CH_BACK_LEFT              0x00000010
#define CH_BACK_RIGHT             0x00000020
#define CH_FRONT_LEFT_OF_CENTER   0x00000040
#define CH_FRONT_RIGHT_OF_CENTER  0x00000080
#define CH_BACK_CENTER            0x00000100
#define CH_SIDE_LEFT              0x00000200
#define CH_SIDE_RIGHT             0x00000400
#define CH_TOP_CENTER             0x00000800
#define CH_TOP_FRONT_LEFT         0x00001000
#define CH_TOP_FRONT_CENTER       0x00002000
#define CH_TOP_FRONT_RIGHT        0x00004000
#define CH_TOP_BACK_LEFT          0x00008000
#define CH_TOP_BACK_CENTER        0x00010000
#define CH_TOP_BACK_RIGHT         0x00020000
#define CH_STEREO_LEFT            0x20000000  ///< Stereo downmix.
#define CH_STEREO_RIGHT           0x40000000  ///< See CH_STEREO_LEFT.
#endif

enum AudioFlags
{
    AUDIO_FLAG_NONE = 0,
    AUDIO_FLAG_EOF = 1
};

typedef struct audio_packet_t {
    int64_t         dts;
    float         **data;
    int             size;
    unsigned        channels;
    unsigned        samplecount;
    uint8_t        *rawdata;
    int             rawsize;
    int64_t         pos;
    enum AudioFlags flags;
    hnd_t           priv;
    hnd_t           owner;
} audio_packet_t;

typedef struct audio_filter_t
{
    int (*init)( hnd_t *handle, const char *opts );
    struct audio_packet_t *(*get_samples)( hnd_t handle, int64_t first_sample, int64_t last_sample );
    void (*free_packet)( hnd_t self, struct audio_packet_t *frame );
    void (*close)( hnd_t handle );
    char *name, *longname, *description, *help;
    void (*help_callback)( int longhelp );
} audio_filter_t;

typedef struct audio_info_t
{
    char    *codec_name;
    int     samplerate; // Sample Rate in Hz
    int     channels;   // How many channels
    int64_t chanlayout; // Channel layout (CH_*)
    int     framelen;   // Frame length in samples
    size_t  framesize;  // Frame size in bytes
    int     chansize;   // Bytes per channel per sample (from the encoded audio)
    int     samplesize; // Bytes per sample (from the encoded audio)
    int64_t time_base_num, time_base_den;
    uint8_t *extradata;
    int     extradata_size;
} audio_info_t;

#include "audio/audio.h"

audio_info_t *x264_af_get_info( hnd_t handle );
audio_filter_t *x264_af_get_filter( char *name );
audio_packet_t *x264_af_get_samples( hnd_t handle, int64_t first_sample, int64_t last_sample );
void x264_af_free_packet( audio_packet_t *pkt );
void x264_af_close( hnd_t chain );

#endif /* AUDIO_H_ */
