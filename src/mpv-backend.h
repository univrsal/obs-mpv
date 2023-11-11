#pragma once
#include "mpv-source.h"
#include "plugin-support.h"
#include <mpv/client.h>
#include <obs-module.h>

enum mpv_command_replies {
    MPVS_PLAYLIST_LOADED = 0x10000,
};

enum mpv_track_type {
    MPV_TRACK_TYPE_AUDIO,
    MPV_TRACK_TYPE_VIDEO,
    MPV_TRACK_TYPE_SUB,
};

struct mpv_track_info {
    int64_t id;
    enum mpv_track_type type;
    char* lang;
    char* title;
    char* decoder_desc;
    bool is_default;
    bool is_selected;
    int64_t demux_w;
    int64_t demux_h;
    int64_t demux_sample_rate;
    int64_t demux_bitrate;
    double pixel_aspect;
    double fps;

    enum speaker_layout demux_channels;
};

extern const char* audio_backends[];
extern size_t audio_backends_count;

/* MPV util macros --------------------------------------------------------- */

#define MPV_VERBOSE_LOGGING 0

#if defined(NDEBUG)
#    define MPV_LOG_LEVEL "info"
#    define MPV_MIN_LOG_LEVEL MPV_LOG_LEVEL_WARN
#else
#    define MPV_LOG_LEVEL "trace"
#    if MPV_VERBOSE_LOGGING
#        define MPV_MIN_LOG_LEVEL MPV_LOG_LEVEL_TRACE
#    else
#        define MPV_MIN_LOG_LEVEL MPV_LOG_LEVEL_INFO
#    endif
#endif

#if defined(WIN32)
#    define MPVS_DEFAULT_AUDIO_DRIVER "wasapi"
#elif defined(__APPLE__)
#    define MPVS_DEFAULT_AUDIO_DRIVER "coreaudio"
#elif defined(__linux__)
#    define MPVS_DEFAULT_AUDIO_DRIVER "alsa"
#elif defined(__FreeBSD__)
#    define MPVS_DEFAULT_AUDIO_DRIVER "oss"
#elif defined(__OpenBSD__)
#    define MPVS_DEFAULT_AUDIO_DRIVER "sndio"
#endif

#define MPV_SEND_COMMAND_ASYNC(...)                                                                   \
    do {                                                                                              \
        if (!context->init)                                                                           \
            break;                                                                                    \
        int __mpv_result = mpv_command_async(context->mpv, 0, (const char*[]) { __VA_ARGS__, NULL }); \
        if (__mpv_result != 0)                                                                        \
            obs_log(LOG_ERROR, "Failed to run mpv command: %s", mpv_error_string(__mpv_result));      \
    } while (0)

#define MPV_GET_PROP_FLAG(name, out)                                                                       \
    do {                                                                                                   \
        if (!context->init)                                                                                \
            break;                                                                                         \
        int __mpv_result = mpv_get_property(context->mpv, name, MPV_FORMAT_FLAG, &out);                    \
        if (error < 0)                                                                                     \
            obs_log(LOG_ERROR, "Failed to get mpv property %s: %s", name, mpv_error_string(__mpv_result)); \
    } while (0)

#define MPV_SET_PROP_STR(name, val)                                                                        \
    do {                                                                                                   \
        if (!context->init)                                                                                \
            break;                                                                                         \
        int __mpv_result = mpv_set_property_string(context->mpv, name, val);                               \
        if (__mpv_result < 0)                                                                              \
            obs_log(LOG_ERROR, "Failed to set mpv property %s: %s", name, mpv_error_string(__mpv_result)); \
    } while (0)

#define MPV_SET_OPTION(name, val)                                                                        \
    do {                                                                                                 \
        if (!context->init)                                                                              \
            break;                                                                                       \
        int __mpv_result = mpv_set_option_string(context->mpv, name, val);                               \
        if (__mpv_result < 0)                                                                            \
            obs_log(LOG_ERROR, "Failed to set mpv option %s: %s", name, mpv_error_string(__mpv_result)); \
    } while (0)

/* MPV util functions ------------------------------------------------------ */

static inline void destroy_mpv_track_info(struct mpv_track_info* track)
{
    bfree(track->lang);
    bfree(track->title);
    bfree(track->decoder_desc);
    track->lang = NULL;
    track->title = NULL;
    track->decoder_desc = NULL;
}

static inline const char* mpvs_obs_channel_layout_to_mpv(uint32_t* sample_rate)
{
    struct obs_audio_info info = { 0 };
    if (obs_get_audio_info(&info)) {
        *sample_rate = info.samples_per_sec;
        switch (info.speakers) {
        case SPEAKERS_MONO:
            return "mono";
        default:
        case SPEAKERS_UNKNOWN:
        case SPEAKERS_STEREO:
            return "stereo";
        case SPEAKERS_2POINT1:
            return "2.1";
        case SPEAKERS_4POINT0:
            return "4.0";
        case SPEAKERS_4POINT1:
            return "4.1";
        case SPEAKERS_5POINT1:
            return "5.1";
        case SPEAKERS_7POINT1:
            return "7.1";
        }
    }
    *sample_rate = 48000;
    return "stereo";
}

static inline int mpvs_audio_driver_to_index(const char* driver)
{
    for (int i = 0; audio_backends[i]; i++) {
        if (strcmp(driver, audio_backends[i]) == 0)
            return i;
    }
    return -1;
}

static inline void mpvs_set_audio_backend(struct mpv_source* context, int backend)
{
    if (backend < 0)
        backend = mpvs_audio_driver_to_index("jack");
    if ((size_t)backend >= audio_backends_count)
        backend = mpvs_audio_driver_to_index(MPVS_DEFAULT_AUDIO_DRIVER);
    MPV_SET_OPTION("ao", audio_backends[backend]);
}

static inline int mpvs_mpv_log_level_to_obs(mpv_log_level lvl)
{
    switch (lvl) {
    case MPV_LOG_LEVEL_FATAL:
    case MPV_LOG_LEVEL_ERROR:
        return LOG_ERROR;
    case MPV_LOG_LEVEL_WARN:
        return LOG_WARNING;
    case MPV_LOG_LEVEL_INFO:
        return LOG_INFO;
    case MPV_LOG_LEVEL_DEBUG:
#if MPV_VERBOSE_LOGGING
    case MPV_LOG_LEVEL_NONE:
    case MPV_LOG_LEVEL_V:
    case MPV_LOG_LEVEL_TRACE:
#endif
    default:
        return LOG_DEBUG;
    }
}

static inline void calc_texture_size(int64_t w, int64_t h, uint32_t* u, uint32_t* v)
{
    *u = (uint32_t)pow(2, ceil(log2((double)w)));
    *v = (uint32_t)pow(2, ceil(log2((double)h)));
}

void mpvs_init_track(struct mpv_source* context, struct mpv_track_info* info, mpv_node* node);

void mpvs_init(struct mpv_source* context);

void mpvs_load_file(struct mpv_source* context, const char* playlist_file);

void mpvs_set_mpv_properties(struct mpv_source* context);

void mpvs_handle_events(struct mpv_source* context);

void mpvs_generate_texture_gl(struct mpv_source* context);

void mpvs_render_gl(struct mpv_source* context);

#if defined(WIN32)
void mpvs_generate_texture_d3d(struct mpv_source* context);

void mpvs_render_d3d(struct mpv_source* context);

void mpvs_render_d3d_shared(struct mpv_source* context);
#else
// stubs for other platforms
static inline void mpvs_generate_texture_d3d(struct mpv_source* context)
{
    UNUSED_PARAMETER(context);
}

static inline void mpvs_render_d3d(struct mpv_source* context);
{
    UNUSED_PARAMETER(context);
}

static inline void mpvs_render_d3d_shared(struct mpv_source* context);
{
    UNUSED_PARAMETER(context);
}
#endif