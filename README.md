## obs-mpv

Adds a video source for OBS Studio using libmpv.

![screenshot](./screenshot.png)

Things left to fix/address
- Add support for playlists, currently it plays only one file
- Add support for switching subtitle and audio tracks
- Add option to load a custom mpv.conf file and the option to set mpv properties/settings
- Audio control
    - MPV does not seem to offer any way of retrieving raw audio via a callback
        - On linux the source creates a jack audio capture and tells mpv to connect to it
          this works well and allows us to control and filter the audio in obs, however
          it does depend on jack or pipewire working. An option to choose beteween
          the different audio backends (oss, sndio, alsa, pulse, jack and pipewire)
          might be useful.
        - Both macOS and Windows would probably just have the option of playing
          audio through the default audio device
- The interact GUI works only for mouse movements, it does not react to clicks
- Neither macOS nor Windows are currently supported
    - macOS is technically not too difficult since obs also uses OpenGL on macOS
      but my rule of thumb with plugins for mac are if I can't get the CI to build
      with a few commits I just don't bother
    - On Windows OBS uses D3D for rendering. While MPV does support Direct3D,
      libmpv does not offer an option to use it, so we'd either have to modify
      libmpv to offer the option of using d3d or render with OpenGL and then copy
      into the D3D texture. I have not bothered looking into this
