#include "mpv-backend.h"
#include "wgl.h"
#include <obs-module.h>
#include <util/darray.h>
#include <util/dstr.h>

const char* audio_backends[] = {
#if defined(__linux__)
    "alsa",
#elif defined(__APPLE__)
    "coreaudio",
#elif defined(_WIN32)
    "wasapi",
#endif
// linux and bsd
#if defined(__linux__) || defined(__FreeBSD__) || defined(__OpenBSD__)
    "pipewire",
    "oss",
    "pulse",
#endif
#if defined(__OpenBSD__)
    "sndio",
#endif
    "sdl",
    "openal",
    "jack",
    NULL
};

size_t audio_backends_count = sizeof(audio_backends) / sizeof(audio_backends[0]) - 1;

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
    void* addr = GLAD_GET_PROC_ADDR(name);
    return addr;
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

    for (size_t i = 0; i < context->tracks.num; i++)
        destroy_mpv_track_info(&context->tracks.array[i]);
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
    struct mpv_track_info sub_track = { 0 };
    sub_track.id = 0;
    sub_track.type = MPV_TRACK_TYPE_SUB;
    sub_track.title = bstrdup(obs_module_text("None"));
    da_push_back(context->tracks, &sub_track);

    // make sure that the current track is less than the number of tracks
    context->current_audio_track = util_clamp(context->current_audio_track, 0, context->audio_tracks - 1);
    context->current_video_track = util_clamp(context->current_video_track, 0, context->video_tracks - 1);
    context->current_sub_track = util_clamp(context->current_sub_track, 0, context->sub_tracks - 1);
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

void mpvs_handle_events(struct mpv_source* context)
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
                if (strlen(txt) > 0)
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
                context->width = (uint32_t)w;
                context->height = (uint32_t)h;
#if defined(WIN32)
                if (obs_device_type == GS_DEVICE_DIRECT3D_11) {
                    calc_texture_size(w, h, &context->d3d_width, &context->d3d_height);
                } else {
                    context->d3d_height = context->height;
                    context->d3d_width = context->width;
                }
#else
                context->d3d_height = context->height;
                context->d3d_width = context->width;
#endif
                context->generate_texture(context);
            }
        } else if (event->event_id == MPV_EVENT_START_FILE) {
            os_atomic_store_long(&context->media_state, OBS_MEDIA_STATE_OPENING);
            mpvs_set_mpv_properties(context);
        } else if (event->event_id == MPV_EVENT_FILE_LOADED) {
            context->file_loaded = true;
            os_atomic_store_long(&context->media_state, OBS_MEDIA_STATE_PLAYING);
            mpvs_handle_file_loaded(context);
        } else if (event->event_id == MPV_EVENT_END_FILE) {
            os_atomic_store_long(&context->media_state, OBS_MEDIA_STATE_ENDED);
        } else if (event->event_id == MPV_EVENT_COMMAND_REPLY) {
            if (event->reply_userdata == MPVS_PLAYLIST_LOADED) {
                // make sure that loop/shuffle are set
                if (context->shuffle)
                    MPV_SEND_COMMAND_ASYNC("playlist-shuffle");
                MPV_SEND_COMMAND_ASYNC("set", "loop", context->loop ? "inf" : "no");
                context->redraw = true;
            }
        }

        if (event->error < 0)
            obs_log(LOG_ERROR, "mpv command %s failed: %s", mpv_event_name(event->event_id), mpv_error_string(event->error));
    }
}

void mpvs_init(struct mpv_source* context)
{
    if (context->init_failed)
        return;

    if (obs_device_type == GS_DEVICE_OPENGL) {
        context->render = mpvs_render_gl;
        context->generate_texture = mpvs_generate_texture_gl;
    } else if (obs_device_type == GS_DEVICE_DIRECT3D_11) {
#if defined(WIN32)
        if (!wgl_init()) {
            context->init_failed = true;
            return;
        }
#endif
        context->render = wgl_have_NV_DX_interop ? mpvs_render_d3d_shared : mpvs_render_d3d;
        context->generate_texture = mpvs_generate_texture_d3d;
    }

    context->_glGenFramebuffers = (PFNGLGENFRAMEBUFFERSPROC)GLAD_GET_PROC_ADDR("glGenFramebuffers");
    context->_glDeleteFramebuffers = (PFNGLDELETEFRAMEBUFFERSPROC)GLAD_GET_PROC_ADDR("glDeleteFramebuffers");
    context->_glBindFramebuffer = (PFNGLBINDFRAMEBUFFERPROC)GLAD_GET_PROC_ADDR("glBindFramebuffer");
    context->_glFramebufferTexture2D = (PFNGLFRAMEBUFFERTEXTURE2DPROC)GLAD_GET_PROC_ADDR("glFramebufferTexture2D");
    context->_glGetIntegerv = (PFNGLGETINTEGERVPROC)GLAD_GET_PROC_ADDR("glGetIntegerv");
    context->_glUseProgram = (PFNGLUSEPROGRAMPROC)GLAD_GET_PROC_ADDR("glUseProgram");
    context->_glReadPixels = (PFNGLREADPIXELSPROC)GLAD_GET_PROC_ADDR("glReadPixels");
    context->_glGenTextures = (PFNGLGENTEXTURESPROC)GLAD_GET_PROC_ADDR("glGenTextures");
    context->_glBindTexture = (PFNGLBINDTEXTUREPROC)GLAD_GET_PROC_ADDR("glBindTexture");
    context->_glTexParameteri = (PFNGLTEXPARAMETERIPROC)GLAD_GET_PROC_ADDR("glTexParameteri");
    context->_glDeleteTextures = (PFNGLDELETETEXTURESPROC)GLAD_GET_PROC_ADDR("glDeleteTextures");
    context->_glTexImage2D = (PFNGLTEXIMAGE2DPROC)GLAD_GET_PROC_ADDR("glTexImage2D");

    context->width = 64; // doesn't matter, this'll change once mpv loads a file and tells us the size
    context->height = 64;
    context->d3d_width = 64;
    context->d3d_height = 64;
    context->generate_texture(context);

    context->mpv = mpv_create();

    MPV_SET_OPTION("audio-client-name", "OBS");

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
        context->init_failed = true;
        return;
    }

    mpv_set_wakeup_callback(context->mpv, handle_mpvs_events, context);
    mpv_render_context_set_update_callback(context->mpv_gl, on_mpvs_render_events, context);

    mpv_observe_property(context->mpv, 0, "playback-time", MPV_FORMAT_DOUBLE);
    mpv_observe_property(context->mpv, 0, "mute", MPV_FORMAT_FLAG);
    mpv_observe_property(context->mpv, 0, "core-idle", MPV_FORMAT_FLAG);
    mpv_observe_property(context->mpv, 0, "idle-active", MPV_FORMAT_FLAG);
    mpv_observe_property(context->mpv, 0, "pause", MPV_FORMAT_FLAG);
    mpv_observe_property(context->mpv, 0, "paused-for-cache", MPV_FORMAT_FLAG);

    if (context->queued_temp_playlist_file_path) {
        mpvs_load_file(context, context->queued_temp_playlist_file_path);
        bfree(context->queued_temp_playlist_file_path);
        context->queued_temp_playlist_file_path = NULL;
    }
    mpvs_set_mpv_properties(context);
    context->init = true;
}

void mpvs_init_track(struct mpv_source* context, struct mpv_track_info* info, mpv_node* node)
{
    mpv_node* value = NULL;
#define MPVS_SET_TRACK_INFO_STRING(id, name)           \
    do {                                               \
        if (strcmp(node->u.list->keys[i], id) == 0) {  \
            if (value->format == MPV_FORMAT_STRING)    \
                info->name = bstrdup(value->u.string); \
        }                                              \
    } while (0)

#define MPVS_SET_TRACK_INFO(id, name, t, val)         \
    do {                                              \
        if (strcmp(node->u.list->keys[i], id) == 0) { \
            if (value->format == t)                   \
                info->name = value->u.val;            \
        }                                             \
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
        } else if (strcmp(node->u.list->keys[i], "demux-channel-count") == 0) {
            if (value->format == MPV_FORMAT_INT64)
                info->demux_channels = value->u.int64;
        }
    }

    struct dstr track_name;
    dstr_init(&track_name);
    switch (info->type) {
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

void mpvs_load_file(struct mpv_source* context, const char* playlist_file)
{
    const char* cmd[] = { "loadfile", playlist_file, NULL };
    int result = mpv_command_async(context->mpv, MPVS_PLAYLIST_LOADED, cmd);

    if (result < 0)
        obs_log(LOG_ERROR, "Failed to load file: %s, %s", playlist_file, mpv_error_string(result));
}

void mpvs_set_mpv_properties(struct mpv_source* context)
{
    // By default mpv will wait in the render callback to exactly hit
    // whatever framerate the playing video has, but we want to render
    // at whatever frame rate obs is using
    MPV_SET_PROP_STR("video-timing-offset", "0");

    // We only want to auto connect if internal audio control is on
    if (mpvs_have_jack_capture_source) {
        if (context->audio_backend < 0 && context->jack_port_name)
            MPV_SET_PROP_STR("jack-port", context->jack_port_name);
        else
            MPV_SET_PROP_STR("jack-port", "");
        if (context->jack_client_name)
            MPV_SET_PROP_STR("jack-name", context->jack_client_name);
    }

    uint32_t sample_rate = 0;
    MPV_SET_PROP_STR("audio-channels", mpvs_obs_channel_layout_to_mpv(&sample_rate));

    // user enabled audio control through obs and a jack audio capture source
    if (context->audio_backend < 0) {
        MPV_SET_PROP_STR("ao", "null");
        MPV_SET_PROP_STR("ao", "jack");
    } else {
        // So if someone switches from internal audio control to jack
        // we have to load the null driver first to make sure mpv knows
        // about the updated jack-port value
        if (context->audio_backend == mpvs_audio_driver_to_index("jack"))
            MPV_SET_PROP_STR("ao", "null");
        MPV_SET_PROP_STR("ao", audio_backends[context->audio_backend]);
    }

    struct dstr str = { 0 };
    dstr_printf(&str, "%d", sample_rate);
    MPV_SET_PROP_STR("audio-samplerate", str.array);
    dstr_free(&str);

    MPV_SET_PROP_STR("osc", context->osc ? "yes" : "no");
    MPV_SET_PROP_STR("input-cursor", context->osc ? "yes" : "no");
    MPV_SET_PROP_STR("input-vo-keyboard", context->osc ? "yes" : "no");
    MPV_SET_PROP_STR("osd-on-seek", context->osc ? "bar" : "no");
}
