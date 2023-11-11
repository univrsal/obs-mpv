#include "mpv-backend.h"
#include "wgl.h"

void mpvs_render_d3d(struct mpv_source* context)
{

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
    gs_blend_state_pop();

    if (result != 0)
        obs_log(LOG_ERROR, "mpv render error: %s", mpv_error_string(result));

    if (context->media_state == OBS_MEDIA_STATE_PLAYING) {
        uint8_t* ptr;
        uint32_t linesize;
        if (gs_texture_map(context->video_buffer, &ptr, &linesize)) {
            context->_glBindFramebuffer(GL_FRAMEBUFFER, context->fbo);
            context->_glReadPixels(0, 0, context->d3d_width, context->d3d_height, GL_RGBA, GL_UNSIGNED_BYTE, ptr);
        }
        gs_texture_unmap(context->video_buffer);
    }
}

void mpvs_render_d3d_shared(struct mpv_source* context)
{
    wgl_lock_shared_texture(context);

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
    gs_blend_state_pop();

    if (result != 0)
        obs_log(LOG_ERROR, "mpv render error: %s", mpv_error_string(result));

    wgl_unlock_shared_texture(context);
}

void mpvs_generate_texture_d3d(struct mpv_source* context)
{
    if (wgl_have_NV_DX_interop)
        wgl_free_shared_gl_texture(context);
    if (context->video_buffer)
        gs_texture_destroy(context->video_buffer);
    context->video_buffer = gs_texture_create(context->d3d_width, context->d3d_height, GS_RGBA, 1, NULL, wgl_have_NV_DX_interop ? 0 : GS_DYNAMIC);

    context->_glBindTexture(GL_TEXTURE_2D, 0);

    if (context->fbo)
        context->_glDeleteFramebuffers(1, &context->fbo);
    if (context->wgl_texture)
        context->_glDeleteTextures(1, &context->wgl_texture);

    context->_glGenTextures(1, &context->wgl_texture);
    context->_glBindTexture(GL_TEXTURE_2D, context->wgl_texture);
    context->_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, context->d3d_width, context->d3d_height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    context->_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    context->_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    context->_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    context->_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    context->_glGenFramebuffers(1, &context->fbo);
    context->_glBindFramebuffer(GL_FRAMEBUFFER, context->fbo);
    context->_glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, context->wgl_texture, 0);

    if (wgl_have_NV_DX_interop)
        wgl_init_shared_gl_texture(context);
}
