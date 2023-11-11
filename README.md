## obs-mpv

Adds a video source for OBS Studio using libmpv.

![screenshot](./screenshot.png)

[![CI](https://github.com/univrsal/obs-mpv/actions/workflows/push.yaml/badge.svg)](https://github.com/univrsal/obs-mpv/actions/workflows/push.yaml)

Things left to note/fix
- Maybe add option to load a custom mpv.conf file and the option to set mpv properties/settings
- Add option to set behavior for player when source is inactive
- Audio control
    - MPV does not seem to offer any way of retrieving raw audio via a callback
        - On linux the source creates a jack audio capture and tells mpv to connect to it
          this works well and allows us to control and filter the audio in obs, however
          it does depend on jack or pipewire working. An option to choose beteween
          the different audio backends (oss, sndio, alsa, pulse, jack and pipewire)
          might be useful.
        - Windows just plays audio through the default audio device
- The interact GUI works only for mouse movements, it does not react to clicks
- On Windows both Direct3D and OpenGL backends of OBS are supported, if Direct3D is used the plugin will try to use the [WGL_NV_DX_interop](https://registry.khronos.org/OpenGL/extensions/NV/WGL_NV_DX_interop.txt)
  extension, which allows sharing of textures between Direct3D and OpenGL. If the extension is not supported the textures will be copied, which is less efficient.
