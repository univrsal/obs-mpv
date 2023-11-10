#pragma once
#include <glad/glad.h>
#include <glad/glad_egl.h>
#include <mpv/client.h>
#include <mpv/render_gl.h>
#include <obs-module.h>
#include <util/dstr.h>
#include <util/threading.h>

#if defined(WIN32)
#    define GLAD_GET_PROC_ADDR wglGetProcAddress
#else
#    define GLAD_GET_PROC_ADDR eglGetProcAddress
#endif
extern int mpvs_have_jack_capture_source;

#define util_min(a, b) ((a) < (b) ? (a) : (b))
#define util_max(a, b) ((a) > (b) ? (a) : (b))
#define util_clamp(a, min, max) util_min(util_max(a, min), max)

#if defined(_WIN32)
#define TMP_DIR "C:\\Windows\\Temp"
#else
#define TMP_DIR "/tmp"
#endif

#define EXTENSIONS_AUDIO \
	"*.3ga;"         \
	"*.669;"         \
	"*.a52;"         \
	"*.aac;"         \
	"*.ac3;"         \
	"*.adt;"         \
	"*.adts;"        \
	"*.aif;"         \
	"*.aifc;"        \
	"*.aiff;"        \
	"*.amb;"         \
	"*.amr;"         \
	"*.aob;"         \
	"*.ape;"         \
	"*.au;"          \
	"*.awb;"         \
	"*.caf;"         \
	"*.dts;"         \
	"*.flac;"        \
	"*.it;"          \
	"*.kar;"         \
	"*.m4a;"         \
	"*.m4b;"         \
	"*.m4p;"         \
	"*.m5p;"         \
	"*.mid;"         \
	"*.mka;"         \
	"*.mlp;"         \
	"*.mod;"         \
	"*.mpa;"         \
	"*.mp1;"         \
	"*.mp2;"         \
	"*.mp3;"         \
	"*.mpc;"         \
	"*.mpga;"        \
	"*.mus;"         \
	"*.oga;"         \
	"*.ogg;"         \
	"*.oma;"         \
	"*.opus;"        \
	"*.qcp;"         \
	"*.ra;"          \
	"*.rmi;"         \
	"*.s3m;"         \
	"*.sid;"         \
	"*.spx;"         \
	"*.tak;"         \
	"*.thd;"         \
	"*.tta;"         \
	"*.voc;"         \
	"*.vqf;"         \
	"*.w64;"         \
	"*.wav;"         \
	"*.wma;"         \
	"*.wv;"          \
	"*.xa;"          \
	"*.xm"

#define EXTENSIONS_VIDEO                                                       \
	"*.3g2;*.3gp;*.3gp2;*.3gpp;*.amv;*.asf;*.avi;"                         \
	"*.bik;*.bin;*.crf;*.divx;*.drc;*.dv;*.evo;*.f4v;*.flv;*.gvi;*.gxf;"   \
	"*.iso;*.m1v;*.m2v;*.m2t;*.m2ts;*.m4v;*.mkv;*.mov;*.mp2;*.mp2v;*.mp4;" \
	"*.mp4v;*.mpe;*.mpeg;*.mpeg1;*.mpeg2;*.mpeg4;*.mpg;*.mpv2;*.mts;"      \
	"*.mtv;*.mxf;*.mxg;*.nsv;*.nuv;*.ogg;*.ogm;*.ogv;*.ogx;*.ps;*.rec;"    \
	"*.rm;*.rmvb;*.rpl;*.thp;*.tod;*.ts;*.tts;*.txd;*.vob;*.vro;*.webm;"   \
	"*.wm;*.wmv;*.wtv;*.xesc"

#define EXTENSIONS_PLAYLIST                           \
	"*.asx;*.b4s;*.cue;*.ifo;*.m3u;*.m3u8;*.pls;" \
	"*.ram;*.rar;*.sdp;*.vlc;*.xspf;*.wax;*.wvx;*.zip;*.conf"

#define EXTENSIONS_MEDIA \
	EXTENSIONS_VIDEO ";" EXTENSIONS_AUDIO ";" EXTENSIONS_PLAYLIST

struct mpv_source {
    // basic source stuff
    uint32_t width;
    uint32_t height;
    obs_source_t* src;
    bool osc; // mpv on screen controller
    DARRAY(char*) files;
    struct dstr last_path;
    char* tmp_playlist_path;
    bool shuffle;
    bool loop;

    // mpv handles/thread stuff
    mpv_handle* mpv;
    mpv_render_context* mpv_gl;
    gs_texture_t* video_buffer;
    pthread_mutex_t mpv_event_mutex;
    GLuint fbo;
	GLuint wgl_texture; // on windows we need to create a texture for mpv to render to
    bool redraw;
    bool init;
    bool init_failed;
    bool new_events;
    bool file_loaded;
    volatile long media_state;
    int audio_backend;
    // when obs starts up we can't load the playlist since the core isn't initialized yet
    // so we save it here and load it when the core is ready
    char* queued_temp_playlist_file_path;

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
    PFNGLREADPIXELSPROC _glReadPixels;
	PFNGLGENTEXTURESPROC _glGenTextures;
	PFNGLBINDTEXTUREPROC _glBindTexture;
	PFNGLTEXPARAMETERIPROC _glTexParameteri;
	PFNGLTEXIMAGE2DPROC _glTexImage2D;
	PFNGLDELETETEXTURESPROC _glDeleteTextures;

    // jack source for audio
    obs_source_t* jack_source;
    char* jack_port_name;   // name of the jack capture source
    char* jack_client_name; // name of the jack client mpv opens for audio output
};
