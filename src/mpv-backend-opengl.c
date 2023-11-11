#include "mpv-backend.h"

void mpvs_render_gl(struct mpv_source* context)
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
    gs_blend_state_pop();

    if (result != 0)
        obs_log(LOG_ERROR, "mpv render error: %s", mpv_error_string(result));

    context->_glUseProgram(currentProgram);

}

void mpvs_generate_texture_gl(struct mpv_source* context)
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
