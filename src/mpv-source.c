#include <inttypes.h>
#include <obs-frontend-api.h>
#include <obs-module.h>
#include <obs-nix-platform.h>
#include <plugin-support.h>
#include <util/dstr.h>
#include <util/platform.h>

#include "mpv-backend.h"
#include "mpv-source.h"

/* Misc functions ---------------------------------------------------------- */

static inline void create_jack_capture(struct mpv_source* context)
{
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
}

static inline void destroy_jack_source(struct mpv_source* context)
{
    obs_source_release(context->jack_source);
    bfree(context->jack_port_name);
    bfree(context->jack_client_name);
    context->jack_source = NULL;
    context->jack_port_name = NULL;
    context->jack_client_name = NULL;
}

static inline bool mpvs_internal_audio_control_modified(obs_properties_t* props,
    obs_property_t* property,
    obs_data_t* settings)
{
    UNUSED_PARAMETER(property);
    bool internal_audio_control = obs_data_get_bool(settings, "internal_audio_control");
    obs_property_set_enabled(obs_properties_get(props, "audio_driver"), !internal_audio_control);
    return true;
}

static inline bool mpvs_file_changed(obs_properties_t* props,
    obs_property_t* property,
    obs_data_t* settings)
{
    UNUSED_PARAMETER(property);
    const char* file = obs_data_get_string(settings, "file");
    obs_property_t* video_tracks = obs_properties_get(props, "video_track");
    obs_property_t* audio_tracks = obs_properties_get(props, "audio_track");
    obs_property_t* sub_tracks = obs_properties_get(props, "sub_track");

    bool enable = strlen(file) > 0;
    obs_property_set_enabled(video_tracks, enable);
    obs_property_set_enabled(audio_tracks, enable);
    obs_property_set_enabled(sub_tracks, enable);
    return true;
}

/* Basic obs functions ----------------------------------------------------- */

static const char* mpvs_source_get_name(void* unused)
{
    UNUSED_PARAMETER(unused);
    return obs_module_text("MPVSource");
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
    context->audio_backend = mpvs_audio_driver_to_index(MPVS_DEFAULT_AUDIO_DRIVER);

    da_init(context->tracks);
    pthread_mutex_init_value(&context->mpv_event_mutex);

    // add default tracks
    struct dstr track_name;
    dstr_init(&track_name);

    struct mpv_track_info sub_track = { 0 };
    sub_track.id = 0;
    sub_track.type = MPV_TRACK_TYPE_SUB;
    sub_track.title = bstrdup(obs_module_text("None"));
    da_push_back(context->tracks, &sub_track);

    dstr_printf(&track_name, "Audio track %i", 1);
    sub_track.id = 1;
    sub_track.type = MPV_TRACK_TYPE_AUDIO;
    sub_track.title = bstrdup(track_name.array);
    da_push_back(context->tracks, &sub_track);

    dstr_printf(&track_name, "Video track %i", 1);
    sub_track.id = 1;
    sub_track.type = MPV_TRACK_TYPE_VIDEO;
    sub_track.title = bstrdup(track_name.array);
    da_push_back(context->tracks, &sub_track);
    dstr_free(&track_name);

    // generates a selected texture with size 512x512, mpv will tell us the actual size later
    obs_enter_graphics();
    mpvs_generate_texture(context);
    obs_leave_graphics();

    create_jack_capture(context);

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

    destroy_jack_source(context);
    bfree(data);
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

    bool internal_audio_control = obs_data_get_bool(settings, "internal_audio_control");

    if (internal_audio_control && mpvs_have_jack_capture_source) {
        context->audio_backend = -1;
        obs_source_add_active_child(context->src, context->jack_source);
    } else {
        obs_source_remove_active_child(context->src, context->jack_source);
        context->audio_backend = obs_data_get_int(settings, "audio_driver");
    }

    mpvs_set_audio_backend(context, context->audio_backend);

    mpvs_set_mpv_properties(context);
}

static void mpvs_source_defaults(obs_data_t* settings)
{
    obs_data_set_default_string(settings, "file", "");
    obs_data_set_default_bool(settings, "osc", false);
    obs_data_set_default_int(settings, "video_track", 0);
    obs_data_set_default_int(settings, "audio_track", 0);
    obs_data_set_default_int(settings, "sub_track", 0);
    obs_data_set_default_bool(settings, "internal_audio_control", false);
    obs_data_set_default_int(settings, "audio_driver", mpvs_audio_driver_to_index(MPVS_DEFAULT_AUDIO_DRIVER));
}

static obs_properties_t* mpvs_source_properties(void* data)
{
    struct mpv_source* context = data;
    obs_properties_t* props = obs_properties_create();

    struct dstr filter_str = { 0 };
    dstr_copy(&filter_str, "Webm (*.webm)");
    dstr_cat(&filter_str, "all files");
    dstr_cat(&filter_str, " (*.*)");

    obs_property_t* file_prop = obs_properties_add_path(props, "file", obs_module_text("File"), OBS_PATH_FILE, filter_str.array, NULL);
    obs_property_set_modified_callback(file_prop, mpvs_file_changed);

    dstr_free(&filter_str);

    obs_properties_add_bool(props, "osc", obs_module_text("EnableOSC"));

    obs_property_t* video_tracks = obs_properties_add_list(props, "video_track", obs_module_text("VideoTrack"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
    obs_property_t* audio_tracks = obs_properties_add_list(props, "audio_track", obs_module_text("AudioTrack"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
    obs_property_t* sub_tracks = obs_properties_add_list(props, "sub_track", obs_module_text("SubTrack"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);

    obs_property_set_enabled(video_tracks, context->file_loaded);
    obs_property_set_enabled(audio_tracks, context->file_loaded);
    obs_property_set_enabled(sub_tracks, context->file_loaded);

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

    // no point in showing this if the jack source doesn't work
    if (mpvs_have_jack_capture_source) {
        obs_property_t* cb = obs_properties_add_bool(props, "internal_audio_control", obs_module_text("InternalAudioControl"));
        obs_property_set_modified_callback(cb, mpvs_internal_audio_control_modified);
    }

    obs_property_t* audio_driver_list = obs_properties_add_list(props, "audio_driver", obs_module_text("AudioDriver"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);

    for (size_t i = 0; audio_backends[i]; i++) {
        size_t index = obs_property_list_add_int(audio_driver_list, audio_backends[i], i);

        // This source is always created so it can only be null if obs
        // doesn't have the jack plugin
        if (strcmp(audio_backends[i], "jack") == 0 && !context->jack_source) {
            obs_property_list_item_disable(audio_driver_list, index, false);
        }
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
    if (!context->mpv || !context->file_loaded)
        return 0;

    double duration;
    int error;

    error = mpv_get_property(context->mpv, "duration/full", MPV_FORMAT_DOUBLE, &duration);

    if (error < 0) {
        obs_log(LOG_ERROR, "Error getting duration: %s", mpv_error_string(error));
        return 0;
    }
    return floor(duration) * 1000;
}

static int64_t mpvs_get_time(void* data)
{
    struct mpv_source* context = data;
    if (!context->mpv || !context->file_loaded)
        return 0;

    double playback_time;
    int error;

    // playback-time does the same thing as time-pos but works for streaming media
    error = mpv_get_property(context->mpv, "playback-time", MPV_FORMAT_DOUBLE, &playback_time);

    if (error < 0) {
        obs_log(LOG_ERROR, "Error getting playback time: %s", mpv_error_string(error));
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
    struct dstr x, y;
    dstr_init(&x);
    dstr_init(&y);

    // convert position to string
    dstr_printf(&x, "%d", event->x);
    dstr_printf(&y, "%d", event->y);
    MPV_SEND_COMMAND_ASYNC("mouse", x.array, y.array);
    dstr_free(&x);
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
    if (context->jack_source)
        enum_callback(context->src, context->jack_source, param);
}

static void mpvs_source_video_tick(void* data, float seconds)
{
    UNUSED_PARAMETER(seconds);
    struct mpv_source* context = data;
    obs_enter_graphics();

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
    obs_leave_graphics();
}

struct obs_source_info mpv_source_info = {
    .id = "mpvs_source",
    .type = OBS_SOURCE_TYPE_INPUT,
    .output_flags = OBS_SOURCE_DO_NOT_DUPLICATE | OBS_SOURCE_VIDEO | OBS_SOURCE_CONTROLLABLE_MEDIA | OBS_SOURCE_INTERACTION,
    .create = mpvs_source_create,
    .destroy = mpvs_source_destroy,
    .get_defaults = mpvs_source_defaults,
    .update = mpvs_source_update,
    .get_name = mpvs_source_get_name,
    .get_width = mpvs_source_getwidth,
    .get_height = mpvs_source_getheight,
    .video_render = mpvs_source_render,
    .video_tick = mpvs_source_video_tick,
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
