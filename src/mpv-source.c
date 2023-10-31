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

struct mpvs_source {
    uint32_t width;
    uint32_t height;
    obs_source_t* src;
    mpv_handle* mpv;
    mpv_render_context* mpv_gl;
    gs_texture_t* texture;
    pthread_mutex_t mutex;
    const char* file_path;
    GLuint fbo;
    bool redraw;
    bool init;
    bool new_events;
    bool file_loaded;
    PFNGLGENFRAMEBUFFERSPROC gen_fbo_fn;
    PFNGLBINDFRAMEBUFFERPROC bind_fb_fn;
    PFNGLDELETEFRAMEBUFFERSPROC del_fbo_fn;
    PFNGLFRAMEBUFFERTEXTURE2DPROC fbo_tex2_fn;
    PFNGLGETINTEGERVPROC get_int_fn;
    PFNGLUSEPROGRAMPROC use_program_fn;
    gs_effect_t* dummy_shader;
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

/* OBS specific functions -------------------------------------------------- */

static const char* mpvs_source_get_name(void* unused)
{
    UNUSED_PARAMETER(unused);
    return obs_module_text("MPVSource");
}

static void mpvs_source_update(void* data, obs_data_t* settings)
{
    struct mpvs_source* context = data;
    uint32_t width = (uint32_t)obs_data_get_int(settings, "width");
    uint32_t height = (uint32_t)obs_data_get_int(settings, "height");

    context->width = width;
    context->height = height;
    context->file_path = obs_data_get_string(settings, "file");

    obs_enter_graphics();
    char* path = obs_module_file("dummy.effect");
    char* err = NULL;
    context->dummy_shader = gs_effect_create_from_file(path, &err);
    bfree(path);

    if (err) {
        obs_log(LOG_ERROR, "Failed to create dummy shader: %s", err);
    }

    if (context->texture) {
        gs_texture_destroy(context->texture);
        context->del_fbo_fn(1, &context->fbo);
    }
    context->texture = gs_texture_create(width, height, GS_RGBA, 1, NULL, GS_RENDER_TARGET);
    gs_set_render_target(context->texture, NULL);
    context->gen_fbo_fn(1, &context->fbo);

    unsigned int* tex = gs_texture_get_obj(context->texture);
    if (tex) {
        context->bind_fb_fn(GL_FRAMEBUFFER, context->fbo);
        context->fbo_tex2_fn(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, *tex, 0);
    }
    gs_set_render_target(NULL, NULL);
    obs_leave_graphics();
}

static void* mpvs_source_create(obs_data_t* settings, obs_source_t* source)
{
    struct mpvs_source* context = bzalloc(sizeof(struct mpvs_source));
    context->src = source;
    context->redraw = true;
    context->gen_fbo_fn = (PFNGLGENFRAMEBUFFERSPROC) eglGetProcAddress("glGenFramebuffers");
    context->del_fbo_fn = (PFNGLDELETEFRAMEBUFFERSPROC) eglGetProcAddress("glDeleteFramebuffers");
    context->bind_fb_fn = (PFNGLBINDFRAMEBUFFERPROC) eglGetProcAddress("glBindFramebuffer");
    context->fbo_tex2_fn = (PFNGLFRAMEBUFFERTEXTURE2DPROC) eglGetProcAddress("glFramebufferTexture2D");
    context->get_int_fn = (PFNGLGETINTEGERVPROC) eglGetProcAddress("glGetIntegerv");
    context->use_program_fn = (PFNGLUSEPROGRAMPROC) eglGetProcAddress("glUseProgram");
    pthread_mutex_init_value(&context->mutex);
    obs_source_update(context->src, settings);
    return context;
}

static void mpvs_source_destroy(void* data)
{
    struct mpvs_source* context = data;
    UNUSED_PARAMETER(context);
    mpv_render_context_free(context->mpv_gl);
    mpv_destroy(context->mpv);
    bfree(data);
}

static obs_properties_t* mpvs_source_properties(void* unused)
{
    UNUSED_PARAMETER(unused);

    obs_properties_t* props = obs_properties_create();
    obs_properties_add_int(props, "width",
        obs_module_text("ColorSource.Width"), 0, 4096, 1);

    obs_properties_add_int(props, "height",
        obs_module_text("ColorSource.Height"), 0, 4096, 1);

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

    if (!context->init) {
        context->init = true;
        context->mpv = mpv_create();
        int result = mpv_initialize(context->mpv) < 0;
        if (result < 0) {
            obs_log(LOG_ERROR, "Failed to initialize mpv context: %s", mpv_error_string(result));
            return;
        }
#if defined(NDEBUG)
        mpvs_request_log_messages(context->mpvs, "info");
#else
        mpv_request_log_messages(context->mpv, "trace");
#endif
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
            mpv_set_wakeup_callback(context->mpv, handle_mpvs_events, context);
            mpv_render_context_set_update_callback(context->mpv_gl, on_mpvs_render_events, context);
        }

        if (strlen(context->file_path) > 0) {
            const char* cmd[] = { "loadfile", context->file_path, NULL };
            result = mpv_command_async(context->mpv, 0, cmd);
            if (result < 0) {
                obs_log(LOG_ERROR, "Failed to load file: %s, %s", context->file_path, mpv_error_string(result));
            }
        }
    }

    pthread_mutex_lock(&context->mutex);
    bool need_redraw = context->redraw;
    bool need_poll = context->new_events;
    if (need_redraw)
        context->redraw = false;
    if (need_poll)
        context->new_events = false;
    pthread_mutex_unlock(&context->mutex);
    if (!context->file_loaded && strlen(context->file_path) > 0 && context->init) {
        const char* cmd[] = { "loadfile", context->file_path, NULL };
        int result = mpv_command_async(context->mpv, 0, cmd);
        if (result < 0) {
            obs_log(LOG_ERROR, "Failed to load file: %s, %s", context->file_path, mpv_error_string(result));
        }
        context->file_loaded = true;
    }

    if (need_poll) {
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
            obs_log(LOG_INFO, "event: %s", mpv_event_name(event->event_id));
        }
    }

    if (need_redraw) {
        GLuint currentProgram;
        context->get_int_fn(GL_CURRENT_PROGRAM, (GLint*)&currentProgram);

//        gs_texture_t* rt = gs_get_render_target();
//        gs_zstencil_t* zt = gs_get_zstencil_target();
//        gs_effect_t* e = gs_get_effect();
//        gs_shader_t* ps = gs_get_pixel_shader();
//        gs_shader_t* vs = gs_get_vertex_shader();
//        struct gs_rect rct = {0};
//        gs_get_viewport(&rct);
//        gs_blend_state_push();
//        UNUSED_PARAMETER(e);
//        gs_load_vertexshader(NULL);
//        gs_load_pixelshader(NULL);
//        mpv_render_frame_info inf;
        mpv_render_param params[] = {
            { MPV_RENDER_PARAM_OPENGL_FBO, &(mpv_opengl_fbo) {
                                               .fbo = context->fbo,
                                               .w = context->width,
                                               .h = context->height,
                                           } },
            { MPV_RENDER_PARAM_NEXT_FRAME_INFO, &(mpv_render_frame_info) {
                                                    .flags = 1,
                                                    .target_time = 0,
                                                }},
            { 0 }
        };
        // See render_gl.h on what OpenGL environment mpvs expects, and
        // other API details.
        int result = mpv_render_context_render(context->mpv_gl, params);
        if (result != 0) {
            obs_log(LOG_ERROR, "mpv render error: %s", mpv_error_string(result));
        }
//        gs_blend_state_pop();
//        gs_set_render_target(rt, zt);
//        gs_set_viewport(rct.x, rct.y, rct.cx, rct.cy);;
//        gs_load_pixelshader(ps);
//        gs_load_vertexshader(vs);
        context->use_program_fn(currentProgram);
    }
    /* obs keeps track of what program is loaded and will only call glUseProgram
     * if the requested program is not already loaded. The problem is that MPV
     * will load its own shader programs via OpenGL functions, which obs will
     * be unaware of. We load this dumm shader here to make sure obs loads
     * whatever program it needs to render the scene.
     */

    UNUSED_PARAMETER(effect);
    gs_blend_state_push();
    gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);

    gs_eparam_t* const param = gs_effect_get_param_by_name(effect, "image");
    gs_effect_set_texture_srgb(param, context->texture);

    gs_draw_sprite(context->texture, 0, context->width, context->height);

    gs_blend_state_pop();
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
    obs_data_set_default_int(settings, "width", 400);
    obs_data_set_default_int(settings, "height", 400);
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
