#pragma once
#include <glad/glad.h>
#include <glad/glad_egl.h>
#include <mpv/client.h>
#include <mpv/render_gl.h>
#include <obs-module.h>
#include <util/threading.h>

extern int mpvs_have_jack_capture_source;

#define util_min(a, b) ((a) < (b) ? (a) : (b))
#define util_max(a, b) ((a) > (b) ? (a) : (b))
#define util_clamp(a, min, max) util_min(util_max(a, min), max)

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
    int audio_backend;

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
