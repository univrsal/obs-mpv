#include <glad/glad_egl.h>
#include <glad/glad.h>
#include <mpv/client.h>
#include <mpv/render_gl.h>
#include <obs-module.h>
#include <plugin-support.h>
#include <util/dstr.h>
#include <obs-nix-platform.h>
#include <obs-frontend-api.h>
#include <util/platform.h>
#include <util/threading.h>

#if defined(NDEBUG)
#define MPV_LOG_LEVEL "info"
#else
#define MPV_LOG_LEVEL "trace"
#endif

struct mpvs_source {
    uint32_t width;
    uint32_t height;
    obs_source_t* src;
    mpv_handle* mpv;
    mpv_render_context* mpv_gl;
    gs_texture_t* video_buffer;
    pthread_mutex_t mutex;
    const char* file_path;
    GLuint fbo;
    bool redraw;
    bool init;
    bool init_failed;
    bool new_events;
    bool file_loaded;
    PFNGLGENFRAMEBUFFERSPROC _glGenFramebuffers;
    PFNGLBINDFRAMEBUFFERPROC _glBindFramebuffer;
    PFNGLDELETEFRAMEBUFFERSPROC _glDeleteFramebuffers;
    PFNGLFRAMEBUFFERTEXTURE2DPROC _glFramebufferTexture2D;
    PFNGLGETINTEGERVPROC _glGetIntegerv;
    PFNGLUSEPROGRAMPROC _glUseProgram;
};

/* MPV specific functions -------------------------------------------------- */

static void on_mpvs_render_events(void* ctx)
{
    struct mpvs_source* context = ctx;
    pthread_mutex_lock(&context->mutex);
    uint64_t flags = mpv_render_context_update(context->mpv_gl);
    if (flags & MPV_RENDER_UPDATE_FRAME) {
        context->redraw = true;
    }
    pthread_mutex_unlock(&context->mutex);
}

static void handle_mpvs_events(void* ctx)
{
    struct mpvs_source* context = ctx;
    pthread_mutex_lock(&context->mutex);
    context->new_events = true;
    pthread_mutex_unlock(&context->mutex);
}

static void* get_proc_address_mpvs(void* ctx, const char* name)
{
    UNUSED_PARAMETER(ctx);
    void* addr = eglGetProcAddress(name);
    return addr;
}

/* Misc functions ---------------------------------------------------------- */

static inline void mpvs_load_file(struct mpvs_source* context)
{
    if (strlen(context->file_path) > 0) {
        const char* cmd[] = { "loadfile", context->file_path, NULL };
        int result = mpv_command_async(context->mpv, 0, cmd);
        if (result < 0) {
            obs_log(LOG_ERROR, "Failed to load file: %s, %s", context->file_path, mpv_error_string(result));
        }
    }
}

static inline void mpvs_generate_texture(struct mpvs_source* context)
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

static inline void mpvs_handle_events(struct mpvs_source* context) {
    while (1) {
        mpv_event* event = mpv_wait_event(context->mpv, 0);
        if (event->event_id == MPV_EVENT_NONE)
            break;

        if (event->event_id == MPV_EVENT_LOG_MESSAGE) {
            mpv_event_log_message* msg = event->data;
            if (msg->log_level < MPV_LOG_LEVEL_INFO)
                obs_log(LOG_INFO, "log: %s", msg->text);
            continue;
        }

        if(event->event_id == MPV_EVENT_VIDEO_RECONFIG) {
            // Retrieve the new video size.
            int64_t w, h;
            if (mpv_get_property(context->mpv, "dwidth", MPV_FORMAT_INT64, &w) >= 0 &&
                mpv_get_property(context->mpv, "dheight", MPV_FORMAT_INT64, &h) >= 0 &&
                w > 0 && h > 0)
            {
                context->width = w;
                context->height = h;
                mpvs_generate_texture(context);
            }
        }
        obs_log(LOG_INFO, "event: %s", mpv_event_name(event->event_id));
    }
}

static inline void mpvs_render(struct mpvs_source* context)
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
        { MPV_RENDER_PARAM_NEXT_FRAME_INFO, &info},
        { MPV_RENDER_PARAM_BLOCK_FOR_TARGET_TIME, &(int) { 1 }},
        { 0 }
    };

    gs_blend_state_push();

    uint64_t start = os_gettime_ns();
    int result = mpv_render_context_render(context->mpv_gl, params);
    if (result != 0) {
        obs_log(LOG_ERROR, "mpv render error: %s", mpv_error_string(result));
    } else {
        uint64_t end = os_gettime_ns();
        // print the time it took to render the frame in ms
        obs_log(LOG_INFO, "render time: %fms", (end - start) / 1000000.0);
    }

    gs_blend_state_pop();

    context->_glUseProgram(currentProgram);
}

static void mpvs_init(struct mpvs_source* context)
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
        { MPV_RENDER_PARAM_ADVANCED_CONTROL, &(int) { 1 } },
        { 0 }
    };

    result = mpv_render_context_create(&context->mpv_gl, context->mpv, params);
    if (result != 0) {
        obs_log(LOG_ERROR, "Failed to initialize mpvs GL context: %s", mpv_error_string(result));
    } else {
        result = mpv_set_property_string(context->mpv, "video-timing-offset", "0");
        if (result != 0)
            obs_log(LOG_ERROR, "Failed to set video-timing-offset: %s", mpv_error_string(result));
        mpv_set_wakeup_callback(context->mpv, handle_mpvs_events, context);
        mpv_render_context_set_update_callback(context->mpv_gl, on_mpvs_render_events, context);
    }

    mpvs_load_file(context);
}

/* OBS specific functions -------------------------------------------------- */

static const char* mpvs_source_get_name(void* unused)
{
    UNUSED_PARAMETER(unused);
    return obs_module_text("MPVSource");
}

static void mpvs_source_update(void* data, obs_data_t* settings)
{
    struct mpvs_source* context = data;
    const char* path = obs_data_get_string(settings, "file");
    if (!context->file_path || strcmp(path, context->file_path) != 0) {
        context->file_loaded = false;
        context->file_path = path;
        if (context->init)
            mpvs_load_file(context);
    }
}

static void* mpvs_source_create(obs_data_t* settings, obs_source_t* source)
{
    struct mpvs_source* context = bzalloc(sizeof(struct mpvs_source));
    context->width = 512;
    context->height = 512;
    context->src = source;
    context->redraw = true;
    context->_glGenFramebuffers = (PFNGLGENFRAMEBUFFERSPROC) eglGetProcAddress("glGenFramebuffers");
    context->_glDeleteFramebuffers = (PFNGLDELETEFRAMEBUFFERSPROC) eglGetProcAddress("glDeleteFramebuffers");
    context->_glBindFramebuffer = (PFNGLBINDFRAMEBUFFERPROC) eglGetProcAddress("glBindFramebuffer");
    context->_glFramebufferTexture2D = (PFNGLFRAMEBUFFERTEXTURE2DPROC) eglGetProcAddress("glFramebufferTexture2D");
    context->_glGetIntegerv = (PFNGLGETINTEGERVPROC) eglGetProcAddress("glGetIntegerv");
    context->_glUseProgram = (PFNGLUSEPROGRAMPROC) eglGetProcAddress("glUseProgram");

    pthread_mutex_init_value(&context->mutex);

    // generates a default texture with size 512x512, mpv will tell us the acutal size later
    obs_enter_graphics();
    mpvs_generate_texture(context);
    obs_leave_graphics();

    obs_source_update(context->src, settings);
    return context;
}

static void mpvs_source_destroy(void* data)
{
    struct mpvs_source* context = data;
    mpv_render_context_free(context->mpv_gl);
    mpv_destroy(context->mpv);

    obs_enter_graphics();
    if (context->video_buffer) {
        if (context->fbo)
            context->_glDeleteFramebuffers(1, &context->fbo);
        gs_texture_destroy(context->video_buffer);
    }
    obs_leave_graphics();

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

    return props;
}

static void mpvs_source_render(void* data, gs_effect_t* effect)
{
    struct mpvs_source* context = data;
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
    pthread_mutex_lock(&context->mutex);
    bool need_redraw = context->redraw;
    bool need_poll = context->new_events;
    if (need_redraw)
        context->redraw = false;
    if (need_poll)
        context->new_events = false;
    pthread_mutex_unlock(&context->mutex);

    if (need_poll)
        mpvs_handle_events(context);


    if (context->init && need_redraw)
        mpvs_render(context);

}

static void mpvs_source_tick(void* data, float seconds)
{
    UNUSED_PARAMETER(data);
    UNUSED_PARAMETER(seconds);
}

static uint32_t mpvs_source_getwidth(void* data)
{
    struct mpvs_source* context = data;
    return context->width;
}

static uint32_t mpvs_source_getheight(void* data)
{
    struct mpvs_source* context = data;
    return context->height;
}

static void mpvs_source_defaults(obs_data_t* settings)
{
    UNUSED_PARAMETER(settings);
}

static void mpvs_play_pause(void* data, bool pause)
{
    struct mpvs_source* context = data;
    UNUSED_PARAMETER(context);
    if (pause) {

    } else {
    }
    UNUSED_PARAMETER(data);
}

static void mpvs_restart(void* data)
{
    UNUSED_PARAMETER(data);
}

static void mpvs_stop(void* data)
{
    UNUSED_PARAMETER(data);
}

static void mpvs_playlist_next(void* data)
{
    UNUSED_PARAMETER(data);
}

static void mpvs_playlist_prev(void* data)
{
    UNUSED_PARAMETER(data);
}

static int64_t mpvs_get_duration(void* data)
{
    UNUSED_PARAMETER(data);
    return 5000;
}

static int64_t mpvs_get_time(void* data)
{
    UNUSED_PARAMETER(data);
    return 500;
}

static void mpvs_set_time(void* data, int64_t ms)
{
    UNUSED_PARAMETER(ms);
    UNUSED_PARAMETER(data);
}

static enum obs_media_state mpvs_get_state(void* data)
{
    UNUSED_PARAMETER(data);
    return OBS_MEDIA_STATE_PLAYING;
}

struct obs_source_info mpv_source_info = {
    .id = "mpvs_source",
    .type = OBS_SOURCE_TYPE_INPUT,
    .output_flags = OBS_SOURCE_ASYNC_VIDEO | OBS_SOURCE_AUDIO | OBS_SOURCE_DO_NOT_DUPLICATE | OBS_SOURCE_CONTROLLABLE_MEDIA,
    .create = mpvs_source_create,
    .destroy = mpvs_source_destroy,
    .update = mpvs_source_update,
    .get_name = mpvs_source_get_name,
    .get_defaults = mpvs_source_defaults,
    .get_width = mpvs_source_getwidth,
    .get_height = mpvs_source_getheight,
    .video_render = mpvs_source_render,
    .video_tick = mpvs_source_tick,
    .get_properties = mpvs_source_properties,
    .icon_type = OBS_ICON_TYPE_MEDIA,
    .media_play_pause = mpvs_play_pause,
    .media_restart = mpvs_restart,
    .media_stop = mpvs_stop,
    .media_next = mpvs_playlist_next,
    .media_previous = mpvs_playlist_prev,
    .media_get_duration = mpvs_get_duration,
    .media_get_time = mpvs_get_time,
    .media_set_time = mpvs_set_time,
    .media_get_state = mpvs_get_state,
};
