/*
obs-mpv
Copyright (C) 2023 Alex uni@vrsal.xyz

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/

#include <glad/glad.h>
#include <glad/glad_egl.h>
#include <obs-module.h>
#include <plugin-support.h>
#include "wgl.h"

OBS_DECLARE_MODULE()
extern struct obs_source_info mpv_source_info;
int mpvs_have_jack_capture_source = 0;

lookup_t* vlc_video_lookup = NULL;
lookup_t* obs_module_lookup = NULL;

/* Forwards lookups to the vlc-video module which has the strings for the
 * portion of this source
 */
const char* obs_module_text(const char* val)
{
    const char* out = val;
    if (!text_lookup_getstr(vlc_video_lookup, val, &out))
        text_lookup_getstr(obs_module_lookup, val, &out);
    return out;
}

void obs_module_set_locale(const char* locale)
{
    if (vlc_video_lookup)
        text_lookup_destroy(vlc_video_lookup);
    if (obs_module_lookup)
        text_lookup_destroy(obs_module_lookup);
    vlc_video_lookup = obs_module_load_locale(obs_get_module("vlc-video"), "en-US", locale);
    obs_module_lookup = obs_module_load_locale(obs_current_module(), "en-US", locale);
}

void obs_module_free_locale(void)
{
    text_lookup_destroy(vlc_video_lookup);
    text_lookup_destroy(obs_module_lookup);
    vlc_video_lookup = NULL;
    obs_module_lookup = NULL;
}

bool obs_module_load(void)
{
    // init glad
#if defined(WIN32)
    if (!wgl_init())
        return false;
#else
    gladLoadEGL();
#endif
    obs_register_source(&mpv_source_info);
    obs_log(LOG_INFO, "plugin loaded successfully (version %s)",
        PLUGIN_VERSION);

    obs_data_t* data = obs_data_create();
    obs_source_t* src = obs_source_create_private("jack_output_capture", "obs-mpv-jack-capture-test", data);
    mpvs_have_jack_capture_source = !!src;
    obs_source_release(src);
    obs_data_release(data);

    return true;
}

void obs_module_unload(void)
{
    obs_log(LOG_INFO, "plugin unloaded");
#if defined(WIN32)
    wgl_deinit();
#else
    gladUnloadEGL();
#endif
}
