#pragma once
#include <stdbool.h>

#if defined(WIN32)
#    include "mpv-source.h"
#    include <Windows.h>
#    include <glad/glad_wgl.h>

extern bool wgl_have_NV_DX_interop;
extern HANDLE wgl_dx_device;

static inline void wgl_lock_shared_texture(void* context)
{
    struct mpv_source* src = context;
    wglDXLockObjectsNV(wgl_dx_device, 1, &src->gl_shared_texture_handle);
}

static inline void wgl_unlock_shared_texture(void* context)
{
    struct mpv_source* src = context;
    wglDXUnlockObjectsNV(wgl_dx_device, 1, &src->gl_shared_texture_handle);
}

#else
#    define wgl_have_NV_DX_interop 0

static inline void wgl_lock_shared_texture(void* context)
{
    UNUSED_PARAMETER(context);
}

static inline void wgl_unlock_shared_texture(void* context)
{
    UNUSED_PARAMETER(context);
}
#endif

bool wgl_init();

void wgl_deinit();

bool wgl_enter_context();

void wgl_exit_context();

void wgl_init_shared_gl_texture(void* context);

void wgl_free_shared_gl_texture(void* context);
