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

#define MPV_VERBOSE_LOGGING 0

#if defined(NDEBUG)
#    define MPV_LOG_LEVEL "info"
#    define MPV_MIN_LOG_LEVEL MPV_LOG_LEVEL_WARN
#else
#    define MPV_LOG_LEVEL "trace"
#    define MPV_MIN_LOG_LEVEL MPV_LOG_LEVEL_INFO
#endif

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

    // gl functions
    PFNGLGENFRAMEBUFFERSPROC _glGenFramebuffers;
    PFNGLBINDFRAMEBUFFERPROC _glBindFramebuffer;
    PFNGLDELETEFRAMEBUFFERSPROC _glDeleteFramebuffers;
    PFNGLFRAMEBUFFERTEXTURE2DPROC _glFramebufferTexture2D;
    PFNGLGETINTEGERVPROC _glGetIntegerv;
    PFNGLUSEPROGRAMPROC _glUseProgram;

    // jack source for audio
    obs_source_t* jack_source;
    char* jack_port_name; // name of the jack capture source
    char* jack_client_name; // name of the jack client mpv opens for audio output
};

/* MPV specific functions -------------------------------------------------- */

#define MPV_SEND_COMMAND_ASYNC(...)                                                                   \
    do {                                                                                              \
        if (!context->init)                                                                           \
            break;                                                                                         \
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
    do {                                                                                                   \
        if (!context->init)                                                                                \
            break;                                                                                         \
        int __mpv_result = mpv_set_option_string(context->mpv, name, val);                               \
        if (__mpv_result < 0)                                                                              \
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

static void mpv_audio_callback(void *data, int samples, int sample_format, void *audio_data) {
    struct mpv_source* context = data;
    UNUSED_PARAMETER(samples);
    UNUSED_PARAMETER(sample_format);
    UNUSED_PARAMETER(context);
    UNUSED_PARAMETER(audio_data);
}

/* Misc functions ---------------------------------------------------------- */

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
        }

        if (event->event_id == MPV_EVENT_PROPERTY_CHANGE) {
            mpv_event_property *prop = (mpv_event_property *)event->data;
            if (strcmp(prop->name, "audio-data") == 0) {
                os_breakpoint();
            }
        }

        if (event->event_id == MPV_EVENT_VIDEO_RECONFIG) {
            // Retrieve the new video size.
            int64_t w, h;
            if (mpv_get_property(context->mpv, "dwidth", MPV_FORMAT_INT64, &w) >= 0 && mpv_get_property(context->mpv, "dheight", MPV_FORMAT_INT64, &h) >= 0 && w > 0 && h > 0) {
                context->width = w;
                context->height = h;
                mpvs_generate_texture(context);
            }
        }
        obs_log(LOG_DEBUG, "event: %s", mpv_event_name(event->event_id));
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
        // By default mpv will wait in the render callback to exactly hit
        // whatever framerate the playing video has, but we want to render
        // at whatever frame rate obs is using
        MPV_SET_PROP_STR("video-timing-offset", "0");

        // MPV does not offer any way to directly get the audio data so
        // we use a jack source to get the audio data and make it available to OBS
        // otherwise mpv just outputs to the desktop audio device
        MPV_SET_PROP_STR("ao", "jack");
        MPV_SET_PROP_STR("jack-port", context->jack_port_name);
        MPV_SET_PROP_STR("jack-name", context->jack_client_name);


        mpv_set_wakeup_callback(context->mpv, handle_mpvs_events, context);
        mpv_render_context_set_update_callback(context->mpv_gl, on_mpvs_render_events, context);
    }

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

    MPV_SET_PROP_STR("osc", context->osc ? "yes" : "no");
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

    pthread_mutex_init_value(&context->mpv_event_mutex);

    struct dstr str;
    dstr_init(&str);
    dstr_catf(&str, "%s audio", obs_source_get_name(context->src));

    // defaults are fine
    obs_data_t* data = obs_data_create();
    context->jack_source = obs_source_create("jack_output_capture", str.array, data, NULL);
    obs_data_release(data);
    dstr_insert(&str, 0, "OBS Studio: "); // all jack sources are prefixed with this
    context->jack_port_name = bstrdup(str.array);
    dstr_printf(&str, "obs-mpv: %s", obs_source_get_name(context->src));
    context->jack_client_name = bstrdup(str.array);
    dstr_free(&str);

    // generates a default texture with size 512x512, mpv will tell us the acutal size later
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

    obs_source_release(context->jack_source);
    bfree(context->jack_port_name);
    bfree(context->jack_client_name);
    bfree(data);
}

static obs_properties_t* mpvs_source_properties(void* unused)
{
    UNUSED_PARAMETER(unused);

    obs_properties_t* props = obs_properties_create();

    struct dstr filter_str = { 0 };
    dstr_copy(&filter_str, "Webm (*.webm)");
    dstr_cat(&filter_str, "all files");
    dstr_cat(&filter_str, " (*.*)");

    obs_properties_add_path(props, "file", obs_module_text("File"), OBS_PATH_FILE, filter_str.array, NULL);

    dstr_free(&filter_str);

    obs_properties_add_bool(props, "osc", obs_module_text("EnableOSC"));

    return props;
}

static void mpvs_source_render(void* data, gs_effect_t* effect)
{
    struct mpv_source* context = data;
    gs_blend_state_push();
    gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);

    gs_eparam_t* const param = gs_effect_get_param_by_name(effect, "image");
    gs_effect_set_texture_srgb(param, context->video_buffer);

    gs_draw_sprite(context->video_buffer, 0, context->width, context->height);

    gs_blend_state_pop();

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
    mpv_set_property_string(context->mpv, "pause", pause ? "yes" : "no");
}

static void mpvs_restart(void* data)
{
    struct mpv_source* context = data;
    MPV_SEND_COMMAND_ASYNC("seek", "0", "absolute");
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
    double playback_time;
    int error;

    error = mpv_get_property(context->mpv, "time-pos", MPV_FORMAT_DOUBLE, &playback_time);

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
    int error = 0, paused = 0, core_idle = 0, idle = 0;
    MPV_GET_PROP_FLAG("pause", paused);
    MPV_GET_PROP_FLAG("core-idle", core_idle);
    MPV_GET_PROP_FLAG("idle-active", idle);

    if (paused)
        return OBS_MEDIA_STATE_PAUSED;
    if (core_idle)
        return OBS_MEDIA_STATE_BUFFERING;
    if (idle)
        return OBS_MEDIA_STATE_ENDED;
    if (!paused)
        return OBS_MEDIA_STATE_PLAYING;
    return OBS_MEDIA_STATE_NONE;
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
    obs_log(LOG_INFO, "%i %i", event->x, event->y);
    MPV_SEND_COMMAND_ASYNC("mouse", pos, pos2);
}


static void mpvs_enum_active_sources(void *data,
                obs_source_enum_proc_t enum_callback,
                void *param)
{
    struct mpv_source* context = data;
    enum_callback(context->src, context->jack_source, param);
}

struct obs_source_info mpv_source_info = {
    .id = "mpvs_source",
    .type = OBS_SOURCE_TYPE_INPUT,
    .output_flags = OBS_SOURCE_ASYNC_VIDEO | OBS_SOURCE_DO_NOT_DUPLICATE | OBS_SOURCE_CONTROLLABLE_MEDIA | OBS_SOURCE_INTERACTION,
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
};
