#include <glad/glad.h>
#include <glad/glad_egl.h>
#include <mpv/client.h>
#include <mpv/render_gl.h>
#include <obs-frontend-api.h>
#include <obs-module.h>
#include <obs-nix-platform.h>
#include <plugin-support.h>
#include <util/dstr.h>
#include <util/platform.h>
#include <util/threading.h>
#include <inttypes.h>

#define util_min(a, b) ((a) < (b) ? (a) : (b))

#define MPV_VERBOSE_LOGGING 0

#if defined(NDEBUG)
#    define MPV_LOG_LEVEL "info"
#    define MPV_MIN_LOG_LEVEL MPV_LOG_LEVEL_WARN
#else
#    define MPV_LOG_LEVEL "trace"
#    define MPV_MIN_LOG_LEVEL MPV_LOG_LEVEL_INFO
#endif

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

struct mpv_source {
    // basic source stuff
    uint32_t width;
    uint32_t height;
    obs_source_t* src;
    bool osc; // mpv on screen controller
    const char* file_path;

    // mpv handles/thread stuff
    mpv_handle* mpv;
    mpv_render_context* mpv_gl;
    gs_texture_t* video_buffer;
    pthread_mutex_t mpv_event_mutex;
    GLuint fbo;
    bool redraw;
    bool init;
    bool init_failed;
    bool new_events;
    bool file_loaded;
    volatile long media_state;

    DARRAY(struct mpv_track_info)
    tracks;
    int audio_tracks;
    int video_tracks;
    int sub_tracks;

    int current_audio_track;
    int current_video_track;
    int current_sub_track;

    // gl functions
    PFNGLGENFRAMEBUFFERSPROC _glGenFramebuffers;
    PFNGLBINDFRAMEBUFFERPROC _glBindFramebuffer;
    PFNGLDELETEFRAMEBUFFERSPROC _glDeleteFramebuffers;
    PFNGLFRAMEBUFFERTEXTURE2DPROC _glFramebufferTexture2D;
    PFNGLGETINTEGERVPROC _glGetIntegerv;
    PFNGLUSEPROGRAMPROC _glUseProgram;

    // jack source for audio
    obs_source_t* jack_source;
    char* jack_port_name;   // name of the jack capture source
    char* jack_client_name; // name of the jack client mpv opens for audio output
};

static inline void destroy_mpv_track_info(struct mpv_track_info* track)
{
    bfree(track->lang);
    bfree(track->title);
    bfree(track->decoder_desc);
    track->lang = NULL;
    track->title = NULL;
    track->decoder_desc = NULL;
}

/* MPV specific functions -------------------------------------------------- */

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

static void on_mpvs_render_events(void* ctx)
{
    struct mpv_source* context = ctx;
    pthread_mutex_lock(&context->mpv_event_mutex);
    uint64_t flags = mpv_render_context_update(context->mpv_gl);
    if (flags & MPV_RENDER_UPDATE_FRAME) {
        context->redraw = true;
    }
    pthread_mutex_unlock(&context->mpv_event_mutex);
}

static void handle_mpvs_events(void* ctx)
{
    struct mpv_source* context = ctx;
    pthread_mutex_lock(&context->mpv_event_mutex);
    context->new_events = true;
    pthread_mutex_unlock(&context->mpv_event_mutex);
}

static void* get_proc_address_mpvs(void* ctx, const char* name)
{
    UNUSED_PARAMETER(ctx);
    void* addr = eglGetProcAddress(name);
    return addr;
}

/* Misc functions ---------------------------------------------------------- */

static inline void mpvs_set_mpv_properties(struct mpv_source* context)
{
    // By selected mpv will wait in the render callback to exactly hit
    // whatever framerate the playing video has, but we want to render
    // at whatever frame rate obs is using
    MPV_SET_PROP_STR("video-timing-offset", "0");

    // MPV does not offer any way to directly get the audio data so
    // we use a jack source to get the audio data and make it available to OBS
    // otherwise mpv just outputs to the desktop audio device
    MPV_SET_PROP_STR("ao", "jack");
    MPV_SET_PROP_STR("jack-port", context->jack_port_name);
    MPV_SET_PROP_STR("jack-name", context->jack_client_name);
    MPV_SET_PROP_STR("audio-channels", "stereo"); // TODO: allow 5.1 etc.

    MPV_SET_PROP_STR("osc", context->osc ? "yes" : "no");
    MPV_SET_PROP_STR("input-cursor", context->osc ? "yes" : "no");
    MPV_SET_PROP_STR("input-vo-keyboard", context->osc ? "yes" : "no");
    MPV_SET_PROP_STR("osd-on-seek", context->osc ? "bar" : "no");
}

static inline void mpvs_load_file(struct mpv_source* context)
{
    if (strlen(context->file_path) > 0) {
        const char* cmd[] = { "loadfile", context->file_path, NULL };
        int result = mpv_command_async(context->mpv, 0, cmd);
        if (result < 0) {
            obs_log(LOG_ERROR, "Failed to load file: %s, %s", context->file_path, mpv_error_string(result));
        }
    }
}

static inline void mpvs_generate_texture(struct mpv_source* context)
{
    if (context->video_buffer) {
        gs_texture_destroy(context->video_buffer);
        context->_glDeleteFramebuffers(1, &context->fbo);
    }
    context->video_buffer = gs_texture_create(context->width, context->height, GS_RGBA, 1, NULL, GS_RENDER_TARGET);
    gs_set_render_target(context->video_buffer, NULL);
    if (context->fbo)
        context->_glDeleteFramebuffers(1, &context->fbo);
    context->_glGenFramebuffers(1, &context->fbo);

    unsigned int* tex = gs_texture_get_obj(context->video_buffer);
    if (tex) {
        context->_glBindFramebuffer(GL_FRAMEBUFFER, context->fbo);
        context->_glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, *tex, 0);
    }
    gs_set_render_target(NULL, NULL);
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

static inline void mpvs_init_track(struct mpv_source* context, struct mpv_track_info* info, mpv_node* node)
{
    mpv_node* value = NULL;
#define MPVS_SET_TRACK_INFO_STRING(id, name) \
    do {                                                       \
        if (strcmp(node->u.list->keys[i], id) == 0) { \
            if (value->format == MPV_FORMAT_STRING)            \
                info->name = bstrdup(value->u.string);         \
        }                                                      \
    } while (0)

#define MPVS_SET_TRACK_INFO(id, name, t, val)                  \
    do {                                                       \
        if (strcmp(node->u.list->keys[i], id) == 0) { \
            if (value->format == t)                            \
                info->name = value->u.val;                     \
        }                                                      \
    } while (0)

#define MPVS_SET_TRACK_INFO_INT64(id, name) \
    MPVS_SET_TRACK_INFO(id, name, MPV_FORMAT_INT64, int64)
#define MPVS_SET_TRACK_INFO_DOUBLE(id, name) \
    MPVS_SET_TRACK_INFO(id, name, MPV_FORMAT_DOUBLE, double_)

    for (int i = 0; i < node->u.list->num; i++) {
        value = &node->u.list->values[i];
        if (!value)
            continue;
        MPVS_SET_TRACK_INFO_INT64("id", id);
        MPVS_SET_TRACK_INFO_STRING("lang", lang);
        MPVS_SET_TRACK_INFO_STRING("title", title);
        MPVS_SET_TRACK_INFO_STRING("decoder-desc", decoder_desc);
        MPVS_SET_TRACK_INFO_INT64("default", is_default);
        MPVS_SET_TRACK_INFO_INT64("selected", is_selected);
        MPVS_SET_TRACK_INFO_INT64("demux-w", demux_w);
        MPVS_SET_TRACK_INFO_INT64("demux-h", demux_h);
        MPVS_SET_TRACK_INFO_INT64("demux-samplerate", demux_sample_rate);
        MPVS_SET_TRACK_INFO_INT64("demux-bitrate", demux_bitrate);
        MPVS_SET_TRACK_INFO_DOUBLE("demux-ar", pixel_aspect);
        MPVS_SET_TRACK_INFO_DOUBLE("demux-fps", fps);

        if (strcmp(node->u.list->keys[i], "type") == 0) {
            if (strcmp(value->u.string, "audio") == 0)
                info->type = MPV_TRACK_TYPE_AUDIO;
            else if (strcmp(value->u.string, "video") == 0)
                info->type = MPV_TRACK_TYPE_VIDEO;
            else if (strcmp(value->u.string, "sub") == 0)
                info->type = MPV_TRACK_TYPE_SUB;
        }
        else if (strcmp(node->u.list->keys[i], "demux-channel-count") == 0) {
            if (value->format == MPV_FORMAT_INT64)
                info->demux_channels = value->u.int64;
        }
    }

    struct dstr track_name;
    dstr_init(&track_name);
    switch(info->type) {
    case MPV_TRACK_TYPE_AUDIO:
        context->audio_tracks++;
        if (!info->title) {
            dstr_catf(&track_name, "Audio track %" PRIu64, info->id);
            info->title = bstrdup(track_name.array);
        }
        break;
    case MPV_TRACK_TYPE_VIDEO:
        context->video_tracks++;
        if (!info->title) {
            dstr_catf(&track_name, "Video track %" PRIu64, info->id);
            info->title = bstrdup(track_name.array);
        }
        break;
    case MPV_TRACK_TYPE_SUB:
        context->sub_tracks++;
        if (!info->title) {

            dstr_catf(&track_name, "Subtitle track %" PRIu64, info->id);
            if (info->lang)
                dstr_catf(&track_name, " (%s)", info->lang);
            info->title = bstrdup(track_name.array);
        }
        break;
    }
    dstr_free(&track_name);
}

static inline void mpvs_handle_file_loaded(struct mpv_source* context)
{
    // get audio tracks
    mpv_node tracks = { 0 };
    int error = mpv_get_property(context->mpv, "track-list", MPV_FORMAT_NODE, &tracks);
    if (error < 0) {
        obs_log(LOG_ERROR, "Failed to get audio tracks: %s", mpv_error_string(error));
        goto end;
    }
    if (tracks.format != MPV_FORMAT_NODE_ARRAY) {
        obs_log(LOG_ERROR, "Failed to get audio tracks: track-list is not an array");
        goto end;
    }

    da_resize(context->tracks, tracks.u.list->num);
    context->audio_tracks = 1;
    context->video_tracks = 1;
    context->sub_tracks = 1;


    for (int i = 0; i < tracks.u.list->num; i++) {
        mpv_node* track = &tracks.u.list->values[i];
        if (track->format != MPV_FORMAT_NODE_MAP) {
            obs_log(LOG_ERROR, "Failed to get audio tracks: track-list[%d] is not a map", i);
            goto end;
        }
        struct mpv_track_info* info = &context->tracks.array[i];
        mpvs_init_track(context, info, track);
    }

    // add the default empty sub track
    // empty audio and video don't really work well
    struct mpv_track_info sub_track = {0};
    sub_track.id = 0;
    sub_track.type = MPV_TRACK_TYPE_SUB;
    sub_track.title = bstrdup(obs_module_text("None"));
    da_push_back(context->tracks, &sub_track);

    // make sure that the current track is less than the number of tracks
    context->current_audio_track = util_min(context->current_audio_track, context->audio_tracks - 1);
    context->current_video_track = util_min(context->current_video_track, context->video_tracks - 1);
    context->current_sub_track = util_min(context->current_sub_track, context->sub_tracks - 1);
    struct dstr str;
    dstr_init(&str);

    dstr_printf(&str, "%d", context->current_video_track);
    MPV_SEND_COMMAND_ASYNC("set", "vid", str.array);

    dstr_printf(&str, "%d", context->current_video_track);
    MPV_SEND_COMMAND_ASYNC("set", "vid", str.array);

    dstr_printf(&str, "%d", context->current_sub_track);
    MPV_SEND_COMMAND_ASYNC("set", "sid", str.array);

    dstr_free(&str);

end:
    mpv_free_node_contents(&tracks);
}

static inline void mpvs_handle_property_change(struct mpv_source* context, mpv_event_property* prop)
{
    long media_state = os_atomic_load_long(&context->media_state);
    if (strcmp(prop->name, "core-idle") == 0) {
        if (prop->format == MPV_FORMAT_FLAG) {
            if (*(unsigned*)prop->data && media_state == OBS_MEDIA_STATE_PLAYING)
                os_atomic_store_long(&context->media_state, OBS_MEDIA_STATE_BUFFERING);
            else
                os_atomic_store_long(&context->media_state, OBS_MEDIA_STATE_PLAYING);
        }
    } else if (strcmp(prop->name, "mute") == 0) {
        if (prop->format == MPV_FORMAT_FLAG)
            obs_source_set_muted(context->jack_source, *(unsigned*)prop->data);
    } else if (strcmp(prop->name, "pause") == 0) {
        if (prop->format == MPV_FORMAT_FLAG) {
            if ((bool)*(unsigned*)prop->data)
                os_atomic_store_long(&context->media_state, OBS_MEDIA_STATE_PAUSED);
            else
                os_atomic_store_long(&context->media_state, OBS_MEDIA_STATE_PLAYING);
        }
    } else if (strcmp(prop->name, "paused-for-cache") == 0) {
        if (prop->format == MPV_FORMAT_FLAG) {
            if (*(unsigned*)prop->data && media_state == OBS_MEDIA_STATE_PLAYING)
                obs_log(LOG_WARNING, "[%s] Your network is slow or stuck, please wait a bit", obs_source_get_name(context->src));
        }
    } else if (strcmp(prop->name, "idle-active") == 0) {
        if (prop->format == MPV_FORMAT_FLAG) {
            if (*(unsigned*)prop->data) {
                os_atomic_store_long(&context->media_state, OBS_MEDIA_STATE_ENDED);
            }
        }
    }
}

static inline void mpvs_handle_events(struct mpv_source* context)
{
    while (1) {
        mpv_event* event = mpv_wait_event(context->mpv, 0);
        if (event->event_id == MPV_EVENT_NONE)
            break;

        if (event->event_id == MPV_EVENT_LOG_MESSAGE) {
            mpv_event_log_message* msg = event->data;
            if (msg->log_level <= MPV_MIN_LOG_LEVEL) {
                // remove \n character
                char* txt = bstrdup(msg->text);
                char* end = txt + strlen(txt) - 1;
                if (*end == '\n')
                    *end = '\0';
                obs_log(mpvs_mpv_log_level_to_obs(msg->log_level), "log: %s", txt);
                bfree(txt);
            }
            continue;
        } else if (event->event_id == MPV_EVENT_PROPERTY_CHANGE) {
            mpvs_handle_property_change(context, (mpv_event_property*)event->data);
        } else if (event->event_id == MPV_EVENT_VIDEO_RECONFIG) {
            // Retrieve the new video size.
            int64_t w, h;
            if (mpv_get_property(context->mpv, "dwidth", MPV_FORMAT_INT64, &w) >= 0 && mpv_get_property(context->mpv, "dheight", MPV_FORMAT_INT64, &h) >= 0 && w > 0 && h > 0) {
                context->width = w;
                context->height = h;
                mpvs_generate_texture(context);
            }
        } else if (event->event_id == MPV_EVENT_START_FILE) {
            os_atomic_store_long(&context->media_state, OBS_MEDIA_STATE_OPENING);
        } else if (event->event_id == MPV_EVENT_FILE_LOADED) {
            context->file_loaded = true;
            os_atomic_store_long(&context->media_state, OBS_MEDIA_STATE_PLAYING);
            mpvs_handle_file_loaded(context);
        } else if (event->event_id == MPV_EVENT_END_FILE) {
            os_atomic_store_long(&context->media_state, OBS_MEDIA_STATE_ENDED);
        }
        if (event->error < 0) {
            obs_log(LOG_ERROR, "mpv command %s failed: %s", mpv_event_name(event->event_id), mpv_error_string(event->error));
        }
    }
}

static inline void mpvs_render(struct mpv_source* context)
{
    // make sure that we restore the current program after mpv is done
    // as obs will not load the progam because it internally keeps track
    // of the current program and only loads it if it has changed
    GLuint currentProgram;
    context->_glGetIntegerv(GL_CURRENT_PROGRAM, (GLint*)&currentProgram);
    mpv_render_frame_info info;

    mpv_render_param params[] = {
        { MPV_RENDER_PARAM_OPENGL_FBO, &(mpv_opengl_fbo) {
                                           .fbo = context->fbo,
                                           .w = context->width,
                                           .h = context->height,
                                       } },
        { MPV_RENDER_PARAM_NEXT_FRAME_INFO, &info }, { MPV_RENDER_PARAM_BLOCK_FOR_TARGET_TIME, &(int) { 1 } }, { 0 }
    };

    gs_blend_state_push();

    int result = mpv_render_context_render(context->mpv_gl, params);
    if (result != 0) {
        obs_log(LOG_ERROR, "mpv render error: %s", mpv_error_string(result));
    }

    gs_blend_state_pop();

    context->_glUseProgram(currentProgram);
}

static void mpvs_init(struct mpv_source* context)
{
    if (context->init_failed)
        return;
    context->init = true;
    context->mpv = mpv_create();

    int result = mpv_initialize(context->mpv) < 0;
    if (result < 0) {
        obs_log(LOG_ERROR, "Failed to initialize mpv context: %s", mpv_error_string(result));
        context->init_failed = true;
        return;
    }

    mpv_request_log_messages(context->mpv, MPV_LOG_LEVEL);

    mpv_render_param params[] = {
        { MPV_RENDER_PARAM_API_TYPE, MPV_RENDER_API_TYPE_OPENGL },
        { MPV_RENDER_PARAM_OPENGL_INIT_PARAMS, &(mpv_opengl_init_params) {
                                                   .get_proc_address = get_proc_address_mpvs,
                                               } },
        { MPV_RENDER_PARAM_ADVANCED_CONTROL, &(int) { 1 } }, { 0 }
    };

    result = mpv_render_context_create(&context->mpv_gl, context->mpv, params);
    if (result != 0) {
        obs_log(LOG_ERROR, "Failed to initialize mpvs GL context: %s", mpv_error_string(result));
    } else {
        mpvs_set_mpv_properties(context);
        mpv_set_wakeup_callback(context->mpv, handle_mpvs_events, context);
        mpv_render_context_set_update_callback(context->mpv_gl, on_mpvs_render_events, context);
    }

    mpv_observe_property(context->mpv, 0, "playback-time", MPV_FORMAT_DOUBLE);
    mpv_observe_property(context->mpv, 0, "mute", MPV_FORMAT_FLAG);
    mpv_observe_property(context->mpv, 0, "core-idle", MPV_FORMAT_FLAG);
    mpv_observe_property(context->mpv, 0, "idle-active", MPV_FORMAT_FLAG);
    mpv_observe_property(context->mpv, 0, "pause", MPV_FORMAT_FLAG);
    mpv_observe_property(context->mpv, 0, "paused-for-cache", MPV_FORMAT_FLAG);

    mpvs_load_file(context);
}

/* Basic obs functions ----------------------------------------------------- */

static const char* mpvs_source_get_name(void* unused)
{
    UNUSED_PARAMETER(unused);
    return obs_module_text("MPVSource");
}

static void mpvs_source_update(void* data, obs_data_t* settings)
{
    struct mpv_source* context = data;
    const char* path = obs_data_get_string(settings, "file");
    context->osc = obs_data_get_bool(settings, "osc");
    if (!context->file_path || strcmp(path, context->file_path) != 0) {
        context->file_loaded = false;
        context->file_path = path;
        if (context->init)
            mpvs_load_file(context);
    }
    mpvs_set_mpv_properties(context);

    int audio_track = obs_data_get_int(settings, "audio_track");
    int video_track = obs_data_get_int(settings, "video_track");
    int sub_track = obs_data_get_int(settings, "sub_track");

    // select the current tracks
    struct dstr str;
    dstr_init(&str);

    if (audio_track != context->current_audio_track) {
        context->current_audio_track = audio_track;
        dstr_printf(&str, "%d", context->current_video_track);
        MPV_SEND_COMMAND_ASYNC("set", "vid", str.array);
    }

    if (video_track != context->current_video_track) {
        context->current_video_track = video_track;
        dstr_printf(&str, "%d", context->current_video_track);
        MPV_SEND_COMMAND_ASYNC("set", "vid", str.array);
    }

    if (sub_track != context->current_sub_track) {
        context->current_sub_track = sub_track;
        dstr_printf(&str, "%d", context->current_sub_track);
        MPV_SEND_COMMAND_ASYNC("set", "sid", str.array);
    }

    dstr_free(&str);
}

static void* mpvs_source_create(obs_data_t* settings, obs_source_t* source)
{
    struct mpv_source* context = bzalloc(sizeof(struct mpv_source));
    context->width = 512;
    context->height = 512;
    context->src = source;
    context->redraw = true;
    context->_glGenFramebuffers = (PFNGLGENFRAMEBUFFERSPROC)eglGetProcAddress("glGenFramebuffers");
    context->_glDeleteFramebuffers = (PFNGLDELETEFRAMEBUFFERSPROC)eglGetProcAddress("glDeleteFramebuffers");
    context->_glBindFramebuffer = (PFNGLBINDFRAMEBUFFERPROC)eglGetProcAddress("glBindFramebuffer");
    context->_glFramebufferTexture2D = (PFNGLFRAMEBUFFERTEXTURE2DPROC)eglGetProcAddress("glFramebufferTexture2D");
    context->_glGetIntegerv = (PFNGLGETINTEGERVPROC)eglGetProcAddress("glGetIntegerv");
    context->_glUseProgram = (PFNGLUSEPROGRAMPROC)eglGetProcAddress("glUseProgram");

    da_init(context->tracks);
    pthread_mutex_init_value(&context->mpv_event_mutex);

    struct dstr str;
    dstr_init(&str);
    dstr_catf(&str, "%s audio", obs_source_get_name(context->src));

    // so for some reason the source already exists every other time you start obs
    // so just reuse it
    context->jack_source = obs_get_source_by_name(str.array);
    if (!context->jack_source) {
        // selecteds are fine
        obs_data_t* data = obs_data_create();
        context->jack_source = obs_source_create("jack_output_capture", str.array, data, NULL);
        obs_data_release(data);
    }
    dstr_insert(&str, 0, "OBS Studio: "); // all jack sources are prefixed with this
    context->jack_port_name = bstrdup(str.array);
    dstr_printf(&str, "obs-mpv: %s", obs_source_get_name(context->src));
    context->jack_client_name = bstrdup(str.array);
    dstr_free(&str);

    // generates a selected texture with size 512x512, mpv will tell us the acutal size later
    obs_enter_graphics();
    mpvs_generate_texture(context);
    obs_leave_graphics();

    obs_source_update(context->src, settings);
    return context;
}

static void mpvs_source_destroy(void* data)
{
    struct mpv_source* context = data;
    mpv_render_context_free(context->mpv_gl);
    mpv_destroy(context->mpv);

    obs_enter_graphics();
    if (context->video_buffer) {
        if (context->fbo)
            context->_glDeleteFramebuffers(1, &context->fbo);
        gs_texture_destroy(context->video_buffer);
    }
    obs_leave_graphics();

    // free all tracks
    for (size_t i = 0; i < context->tracks.num; i++) {
        destroy_mpv_track_info(&context->tracks.array[i]);
    }
    da_free(context->tracks);

    obs_source_release(context->jack_source);
    bfree(context->jack_port_name);
    bfree(context->jack_client_name);
    bfree(data);
}

static obs_properties_t* mpvs_source_properties(void* data)
{
    struct mpv_source* context = data;
    obs_properties_t* props = obs_properties_create();

    struct dstr filter_str = { 0 };
    dstr_copy(&filter_str, "Webm (*.webm)");
    dstr_cat(&filter_str, "all files");
    dstr_cat(&filter_str, " (*.*)");

    obs_properties_add_path(props, "file", obs_module_text("File"), OBS_PATH_FILE, filter_str.array, NULL);

    dstr_free(&filter_str);

    obs_properties_add_bool(props, "osc", obs_module_text("EnableOSC"));

    obs_property_t* video_tracks = obs_properties_add_list(props, "video_track", obs_module_text("VideoTrack"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
    obs_property_t* audio_tracks = obs_properties_add_list(props, "audio_track", obs_module_text("AudioTrack"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
    obs_property_t* sub_tracks = obs_properties_add_list(props, "sub_track", obs_module_text("SubTrack"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);

    obs_property_set_enabled(video_tracks, context->video_tracks > 0);
    obs_property_set_enabled(audio_tracks, context->audio_tracks > 0);
    obs_property_set_enabled(sub_tracks, context->sub_tracks > 0);

    // iterate over all tracks and add them to the list
    for (size_t i = 0; i < context->tracks.num; i++) {
        struct mpv_track_info* track = &context->tracks.array[i];
        if (track->type == MPV_TRACK_TYPE_VIDEO)
            obs_property_list_add_int(video_tracks, track->title, track->id);
        else if (track->type == MPV_TRACK_TYPE_AUDIO)
            obs_property_list_add_int(audio_tracks, track->title, track->id);
        else if (track->type == MPV_TRACK_TYPE_SUB)
            obs_property_list_add_int(sub_tracks, track->title, track->id);
    }

    return props;
}

static void mpvs_source_render(void* data, gs_effect_t* effect)
{
    struct mpv_source* context = data;

    const bool previous = gs_framebuffer_srgb_enabled();
    gs_enable_framebuffer_srgb(true);

    gs_blend_state_push();
    gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);

    gs_eparam_t* const param = gs_effect_get_param_by_name(effect, "image");
    gs_effect_set_texture_srgb(param, context->video_buffer);

    gs_draw_sprite(context->video_buffer, 0, context->width, context->height);

    gs_blend_state_pop();
    gs_enable_framebuffer_srgb(previous);

    if (!context->init)
        mpvs_init(context);
    if (context->init_failed)
        return;

    // mpv will set these flags in a separate thread
    // from what I can tell initilazation, event handling and rendering
    // should all happen in the same thread so we all do it here in the graphics thread
    pthread_mutex_lock(&context->mpv_event_mutex);
    bool need_redraw = context->redraw;
    bool need_poll = context->new_events;
    if (need_redraw)
        context->redraw = false;
    if (need_poll)
        context->new_events = false;
    pthread_mutex_unlock(&context->mpv_event_mutex);

    if (need_poll)
        mpvs_handle_events(context);

    if (context->init && need_redraw)
        mpvs_render(context);
}

static uint32_t mpvs_source_getwidth(void* data)
{
    struct mpv_source* context = data;
    return context->width;
}

static uint32_t mpvs_source_getheight(void* data)
{
    struct mpv_source* context = data;
    return context->height;
}

/* OBS media functions ----------------------------------------------------- */

static void mpvs_play_pause(void* data, bool pause)
{
    struct mpv_source* context = data;

    if (!context->mpv)
        return;
    mpv_set_property_string(context->mpv, "pause", pause ? "yes" : "no");
}

static void mpvs_restart(void* data)
{
    struct mpv_source* context = data;
    mpvs_load_file(context);
}

static void mpvs_stop(void* data)
{
    struct mpv_source* context = data;
    MPV_SEND_COMMAND_ASYNC("stop");
}

static void mpvs_playlist_next(void* data)
{
    struct mpv_source* context = data;
    MPV_SEND_COMMAND_ASYNC("playlist-next");
}

static void mpvs_playlist_prev(void* data)
{
    struct mpv_source* context = data;
    MPV_SEND_COMMAND_ASYNC("playlist-prev");
}

static int64_t mpvs_get_duration(void* data)
{
    struct mpv_source* context = data;
    if (!context->mpv)
        return 0;

    double duration;
    int error;

    error = mpv_get_property(context->mpv, "duration/full", MPV_FORMAT_DOUBLE, &duration);

    if (error < 0) {
        obs_log(LOG_ERROR, "Error getting duration: %s\n", mpv_error_string(error));
        return 0;
    }
    return floor(duration) * 1000;
}

static int64_t mpvs_get_time(void* data)
{
    struct mpv_source* context = data;
    if (!context->mpv)
        return 0;

    double playback_time;
    int error;

    // playback-time does the same thing as time-pos but works for streaming media
    error = mpv_get_property(context->mpv, "playback-time", MPV_FORMAT_DOUBLE, &playback_time);

    if (error < 0) {
        obs_log(LOG_ERROR, "Error getting playback time: %s\n", mpv_error_string(error));
        return 0;
    }
    return floor(playback_time) * 1000;
}

static void mpvs_set_time(void* data, int64_t ms)
{
    struct mpv_source* context = data;
    double time = ms / 1000.0;
    struct dstr str;
    dstr_init(&str);
    dstr_catf(&str, "%.2f", time);
    MPV_SEND_COMMAND_ASYNC("seek", str.array, "absolute");
    dstr_free(&str);
}

static enum obs_media_state mpvs_get_state(void* data)
{
    struct mpv_source* context = data;
    return (enum obs_media_state)os_atomic_load_long(&context->media_state);
}

/* OBS interaction functions ----------------------------------------------- */

static void mpvs_mouse_click(void* data, const struct obs_mouse_event* event,
    int32_t type, bool mouse_up, uint32_t click_count)
{
    struct mpv_source* context = data;
    UNUSED_PARAMETER(context);
    UNUSED_PARAMETER(event);
    UNUSED_PARAMETER(type);
    UNUSED_PARAMETER(mouse_up);
    UNUSED_PARAMETER(click_count);

    DARRAY(mpv_node)
    nodes;
    da_init(nodes);
    da_resize(nodes, 5);
    nodes.array[0].format = MPV_FORMAT_STRING;
    nodes.array[0].u.string = "mouse";
    nodes.array[1].format = MPV_FORMAT_INT64;
    nodes.array[1].u.int64 = event->x;
    nodes.array[2].format = MPV_FORMAT_INT64;
    nodes.array[2].u.int64 = event->y;
    nodes.array[3].format = MPV_FORMAT_INT64;
    nodes.array[3].u.int64 = type;
    nodes.array[4].format = MPV_FORMAT_STRING;
    nodes.array[4].u.string = click_count > 1 ? "double" : "single";

    mpv_node_list list;
    list.num = nodes.num;
    list.values = nodes.array;

    mpv_node main;
    main.format = MPV_FORMAT_NODE_ARRAY;
    main.u.list = &list;

    if (mouse_up) {
        list.num = 3;
        mpv_command_node_async(context->mpv, 0, &main);
    } else {
        mpv_command_node_async(context->mpv, 0, &main);
    }
    da_free(nodes);
}

static void mpvs_mouse_move(void* data, const struct obs_mouse_event* event,
    bool mouse_leave)
{
    struct mpv_source* context = data;
    UNUSED_PARAMETER(mouse_leave);
    // convert position to string
    char pos[32];
    snprintf(pos, sizeof(pos), "%d", event->x);
    char pos2[32];
    snprintf(pos2, sizeof(pos2), "%d", event->y);
    MPV_SEND_COMMAND_ASYNC("mouse", pos, pos2);
}

static void mpvs_key_click(void* data, const struct obs_key_event* event,
    bool key_up)
{
    struct mpv_source* context = data;
    struct dstr key_combo;
    dstr_init(&key_combo);
    const bool mouse_left = event->modifiers & INTERACT_MOUSE_LEFT;
    const bool mouse_right = event->modifiers & INTERACT_MOUSE_RIGHT;
    const bool mouse_middle = event->modifiers & INTERACT_MOUSE_MIDDLE;
    const bool is_mouse_combo = mouse_left || mouse_right || mouse_middle;

    const char* combo = (!!event->text || is_mouse_combo) ? "+" : "";
    if (event->modifiers & INTERACT_SHIFT_KEY)
        dstr_catf(&key_combo, "Shift%s", combo);
    if (event->modifiers & INTERACT_CONTROL_KEY)
        dstr_catf(&key_combo, "Ctrl%s", combo);
    if (event->modifiers & INTERACT_ALT_KEY)
        dstr_catf(&key_combo, "Alt%s", combo);
    if (event->modifiers & INTERACT_COMMAND_KEY)
        dstr_catf(&key_combo, "Meta%s", combo);

    if (is_mouse_combo) {
        if (mouse_left)
            dstr_catf(&key_combo, "MBTN_LEFT");
        else if (mouse_right)
            dstr_catf(&key_combo, "MBTN_RIGHT");
        else if (mouse_middle)
            dstr_catf(&key_combo, "MBTN_MIDDLE");
    } else if (event->text) {
        dstr_catf(&key_combo, "%s", event->text);
    }

    if (key_combo.len == 0)
        return;

    obs_log(LOG_DEBUG, "MPV key combo: %s", key_combo.array);

    if (key_up) {
        MPV_SEND_COMMAND_ASYNC("keyup", key_combo.array);
    } else {
        MPV_SEND_COMMAND_ASYNC("keydown", key_combo.array);
    }
    dstr_free(&key_combo);
}

static void mpvs_enum_active_sources(void* data,
    obs_source_enum_proc_t enum_callback,
    void* param)
{
    struct mpv_source* context = data;
    enum_callback(context->src, context->jack_source, param);
}

struct obs_source_info mpv_source_info = {
    .id = "mpvs_source",
    .type = OBS_SOURCE_TYPE_INPUT,
    .output_flags = OBS_SOURCE_DO_NOT_DUPLICATE | OBS_SOURCE_VIDEO | OBS_SOURCE_CONTROLLABLE_MEDIA | OBS_SOURCE_INTERACTION,
    .create = mpvs_source_create,
    .destroy = mpvs_source_destroy,
    .update = mpvs_source_update,
    .get_name = mpvs_source_get_name,
    .get_width = mpvs_source_getwidth,
    .get_height = mpvs_source_getheight,
    .video_render = mpvs_source_render,
    .get_properties = mpvs_source_properties,
    .icon_type = OBS_ICON_TYPE_MEDIA,
    .enum_active_sources = mpvs_enum_active_sources,

    .media_play_pause = mpvs_play_pause,
    .media_restart = mpvs_restart,
    .media_stop = mpvs_stop,
    .media_next = mpvs_playlist_next,
    .media_previous = mpvs_playlist_prev,
    .media_get_duration = mpvs_get_duration,
    .media_get_time = mpvs_get_time,
    .media_set_time = mpvs_set_time,
    .media_get_state = mpvs_get_state,

    .mouse_click = mpvs_mouse_click,
    .mouse_move = mpvs_mouse_move,
    .key_click = mpvs_key_click,
};
