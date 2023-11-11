#include "wgl.h"
#include <obs-module.h>
#include <plugin-support.h>

WNDCLASSEX wc;
HWND hwnd;
HGLRC hrc;
HDC hdc;
HANDLE wgl_dx_device;

static const char* dummy_window_class = "GLDummyWindow-obs-mpv";
static bool registered_dummy_window_class = false;
bool wgl_have_NV_DX_interop = false;

struct dummy_context {
    HWND hwnd;
    HGLRC hrc;
    HDC hdc;
};

static bool gl_register_dummy_window_class(void)
{
    WNDCLASSA wc;
    if (registered_dummy_window_class)
        return true;

    memset(&wc, 0, sizeof(wc));
    wc.style = CS_OWNDC;
    wc.hInstance = GetModuleHandle(NULL);
    wc.lpfnWndProc = DefWindowProc;
    wc.lpszClassName = dummy_window_class;

    if (!RegisterClassA(&wc)) {
        blog(LOG_ERROR, "Could not create dummy window class");
        return false;
    }

    registered_dummy_window_class = true;
    return true;
}

static inline HWND gl_create_dummy_window(void)
{
    HWND hwnd = CreateWindowExA(0, dummy_window_class, "Dummy GL Window 2",
        WS_POPUP, 0, 0, 2, 2, NULL, NULL,
        GetModuleHandle(NULL), NULL);
    if (!hwnd)
        blog(LOG_ERROR, "Could not create dummy context window");

    return hwnd;
}

static inline void init_dummy_pixel_format(PIXELFORMATDESCRIPTOR* pfd)
{
    memset(pfd, 0, sizeof(PIXELFORMATDESCRIPTOR));
    pfd->nSize = sizeof(PIXELFORMATDESCRIPTOR);
    pfd->nVersion = 1;
    pfd->iPixelType = PFD_TYPE_RGBA;
    pfd->cColorBits = 32;
    pfd->cDepthBits = 24;
    pfd->cStencilBits = 8;
    pfd->iLayerType = PFD_MAIN_PLANE;
    pfd->dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL |
        PFD_DOUBLEBUFFER;
}

static inline HGLRC gl_init_basic_context(HDC hdc)
{
    HGLRC hglrc = wglCreateContext(hdc);
    if (!hglrc) {
        blog(LOG_ERROR, "wglCreateContext failed, %lu", GetLastError());
        return NULL;
    }

    if (!wglMakeCurrent(hdc, hglrc)) {
        wglDeleteContext(hglrc);
        return NULL;
    }

    return hglrc;
}

static bool gl_dummy_context_init(struct dummy_context* dummy)
{
    PIXELFORMATDESCRIPTOR pfd;
    int format_index;

    if (!gl_register_dummy_window_class())
        return false;

    dummy->hwnd = gl_create_dummy_window();
    if (!dummy->hwnd)
        return false;

    dummy->hdc = GetDC(dummy->hwnd);

    init_dummy_pixel_format(&pfd);
    format_index = ChoosePixelFormat(dummy->hdc, &pfd);
    if (!format_index) {
        blog(LOG_ERROR, "Dummy ChoosePixelFormat failed, %lu",
            GetLastError());
        return false;
    }

    if (!SetPixelFormat(dummy->hdc, format_index, &pfd)) {
        blog(LOG_ERROR, "Dummy SetPixelFormat failed, %lu",
            GetLastError());
        return false;
    }

    dummy->hrc = gl_init_basic_context(dummy->hdc);
    if (!dummy->hrc) {
        blog(LOG_ERROR, "Failed to initialize dummy context");
        return false;
    }

    return true;
}

static inline void gl_dummy_context_free(struct dummy_context* dummy)
{
    wglMakeCurrent(NULL, NULL);
    wglDeleteContext(dummy->hrc);
    DestroyWindow(dummy->hwnd);
    memset(dummy, 0, sizeof(struct dummy_context));
}


static bool register_dummy_class(void)
{
    static bool created = false;

    WNDCLASSA wc = { 0 };
    wc.style = CS_OWNDC;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpfnWndProc = (WNDPROC)DefWindowProcA;
    wc.lpszClassName = "obs-mpv dummy class";

    if (created)
        return true;

    if (!RegisterClassA(&wc)) {
        blog(LOG_ERROR, "Failed to register dummy GL window class, %lu",
            GetLastError());
        return false;
    }

    created = true;
    return true;
}

static bool create_dummy_window()
{
    hwnd = CreateWindowExA(0, "obs-mpv dummy class",
        "OpenGL Dummy Window", WS_POPUP, 0,
        0, 1, 1, NULL, NULL,
        GetModuleHandleW(NULL), NULL);
    if (!hwnd) {
        obs_log(LOG_ERROR, "Failed to create dummy GL window, %lu",
            GetLastError());
        return false;
    }

    hdc = GetDC(hwnd);
    if (!hdc) {
        obs_log(LOG_ERROR, "Failed to get dummy GL window DC (%lu)",
            GetLastError());
        return false;
    }

    return true;
}

static inline HGLRC gl_init_context(HDC hdc)
{
    static const int attribs[] = {
#ifdef _DEBUG
        WGL_CONTEXT_FLAGS_ARB,
        WGL_CONTEXT_DEBUG_BIT_ARB,
#endif
        WGL_CONTEXT_PROFILE_MASK_ARB,
        WGL_CONTEXT_CORE_PROFILE_BIT_ARB,
        WGL_CONTEXT_MAJOR_VERSION_ARB,
        3,
        WGL_CONTEXT_MINOR_VERSION_ARB,
        3,
        0,
        0 };

    HGLRC hglrc = wglCreateContextAttribsARB(hdc, 0, attribs);
    if (!hglrc) {
        blog(LOG_ERROR,
            "wglCreateContextAttribsARB failed, "
            "%lu",
            GetLastError());
        return NULL;
    }

    if (!wgl_enter_context()) {
        wglDeleteContext(hglrc);
        return NULL;
    }

    return hglrc;
}


static inline void add_attrib(struct darray* list, int attrib, int val)
{
    darray_push_back(sizeof(int), list, &attrib);
    darray_push_back(sizeof(int), list, &val);
}

static inline int get_color_format_bits(enum gs_color_format format)
{
    switch (format) {
    case GS_RGBA:
    case GS_BGRA:
        return 32;
    default:
        return 0;
    }
}

static inline int get_depth_format_bits(enum gs_zstencil_format zsformat)
{
    switch (zsformat) {
    case GS_Z16:
        return 16;
    case GS_Z24_S8:
        return 24;
    default:
        return 0;
    }
}

static inline int get_stencil_format_bits(enum gs_zstencil_format zsformat)
{
    switch (zsformat) {
    case GS_Z24_S8:
        return 8;
    default:
        return 0;
    }
}

static int gl_choose_pixel_format(HDC hdc, const struct gs_init_data* info)
{
    struct darray attribs;
    int color_bits = get_color_format_bits(info->format);
    int depth_bits = get_depth_format_bits(info->zsformat);
    int stencil_bits = get_stencil_format_bits(info->zsformat);
    UINT num_formats;
    BOOL success;
    int format;

    if (!color_bits) {
        obs_log(LOG_ERROR, "gl_init_pixel_format: color format not "
            "supported");
        return false;
    }

    darray_init(&attribs);
    add_attrib(&attribs, WGL_DRAW_TO_WINDOW_ARB, GL_TRUE);
    add_attrib(&attribs, WGL_SUPPORT_OPENGL_ARB, GL_TRUE);
    add_attrib(&attribs, WGL_ACCELERATION_ARB, WGL_FULL_ACCELERATION_ARB);
    add_attrib(&attribs, WGL_DOUBLE_BUFFER_ARB, GL_TRUE);
    add_attrib(&attribs, WGL_PIXEL_TYPE_ARB, WGL_TYPE_RGBA_ARB);
    add_attrib(&attribs, WGL_COLOR_BITS_ARB, color_bits);
    add_attrib(&attribs, WGL_DEPTH_BITS_ARB, depth_bits);
    add_attrib(&attribs, WGL_STENCIL_BITS_ARB, stencil_bits);
    add_attrib(&attribs, 0, 0);

    success = wglChoosePixelFormatARB(hdc, attribs.array, NULL, 1, &format,
        &num_formats);
    if (!success || !num_formats) {
        blog(LOG_ERROR, "wglChoosePixelFormatARB failed, %lu",
            GetLastError());
        format = 0;
    }

    darray_free(&attribs);

    return format;
}

static inline bool gl_getpixelformat(HDC hdc, const struct gs_init_data* info,
    int* format, PIXELFORMATDESCRIPTOR* pfd)
{
    if (!format)
        return false;

    *format = gl_choose_pixel_format(hdc, info);

    if (!DescribePixelFormat(hdc, *format, sizeof(*pfd), pfd)) {
        blog(LOG_ERROR, "DescribePixelFormat failed, %lu",
            GetLastError());
        return false;
    }

    return true;
}

static inline bool gl_setpixelformat(HDC hdc, int format,
    PIXELFORMATDESCRIPTOR* pfd)
{
    if (!SetPixelFormat(hdc, format, pfd)) {
        blog(LOG_ERROR, "SetPixelFormat failed, %lu", GetLastError());
        return false;
    }

    return true;
}

static inline void required_extension_error(const char* extension)
{
    blog(LOG_ERROR, "OpenGL extension %s is required", extension);
}

static bool gl_init_extensions(HDC hdc)
{
    if (!gladLoadWGL(hdc)) {
        blog(LOG_ERROR, "Failed to load WGL entry functions.");
        return false;
    }

    if (!GLAD_WGL_ARB_pixel_format) {
        required_extension_error("ARB_pixel_format");
        return false;
    }

    if (!GLAD_WGL_ARB_create_context) {
        required_extension_error("ARB_create_context");
        return false;
    }

    if (!GLAD_WGL_ARB_create_context_profile) {
        required_extension_error("ARB_create_context_profile");
        return false;
    }

    return true;
}

bool wgl_init()
{
    static bool initialized = false;
    static bool init_result = false;
    if (initialized)
        return init_result;
    initialized = true;

    struct dummy_context dummy = { 0 };
    struct gs_init_data info = { 0 };
    info.format = gs_get_format_from_space(gs_get_color_space());
    int pixel_format;
    PIXELFORMATDESCRIPTOR pfd;
    if (!gl_dummy_context_init(&dummy))
        goto fail;
    if (!gl_init_extensions(dummy.hdc))
        goto fail;
    if (!register_dummy_class())
        return false;
    if (!create_dummy_window())
        return false;

    if (!gl_getpixelformat(dummy.hdc, &info, &pixel_format, &pfd))
        goto fail;
    gl_dummy_context_free(&dummy);

    if (!gl_setpixelformat(hdc, pixel_format, &pfd))
        goto fail;

    hrc = gl_init_context(hdc);
    if (!wglMakeCurrent(hdc, hrc)) {
        obs_log(LOG_ERROR, "Failed to make dummy GL context current (%lu)",
            GetLastError());
        return false;
    }
    
    // Initialize Glad
    if (!gladLoadGL()) {
        obs_log(LOG_ERROR, "Failed to initialize OpenGL");
        return false;
    }

    const GLubyte* glVersion = glGetString(GL_VERSION);
    if (glVersion) {
        obs_log(LOG_INFO, "OpenGL Version: %s\n", glVersion);
    }

    wgl_dx_device = wglDXOpenDeviceNV(gs_get_device_obj());
    if (wgl_dx_device) {
        wgl_have_NV_DX_interop = true;
        obs_log(LOG_INFO, "NV_DX_interop extension is supported, sharing texture between OpenGL and Direct3D");
    }
    
    init_result = true;
    return true;
fail:
    gl_dummy_context_free(&dummy);
    return false;
}

void wgl_deinit()
{
    wglMakeCurrent(NULL, NULL);
    if (wgl_dx_device)
        wglDXCloseDeviceNV(wgl_dx_device);
    wglDeleteContext(hrc);
    ReleaseDC(hwnd, hdc);
    DestroyWindow(hwnd);
    UnregisterClass(wc.lpszClassName, wc.hInstance);
}

bool wgl_enter_context()
{
    return wglMakeCurrent(hdc, hrc);
}

void wgl_exit_context()
{
    wglMakeCurrent(NULL, NULL);
}

void wgl_init_shared_gl_texture(void* context)
{
    struct mpv_source* src = context;
    src->gl_shared_texture_handle = wglDXRegisterObjectNV(wgl_dx_device,
        gs_texture_get_obj(src->video_buffer),
        src->wgl_texture,
        GL_TEXTURE_2D,
        WGL_ACCESS_WRITE_DISCARD_NV);
}

void wgl_free_shared_gl_texture(void* context)
{
    struct mpv_source* src = context;
    if (src->gl_shared_texture_handle)
        wglDXUnregisterObjectNV(wgl_dx_device, src->gl_shared_texture_handle);
    src->gl_shared_texture_handle = 0;
}
