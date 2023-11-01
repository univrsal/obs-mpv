## obs-mpv

Adds a video source for OBS Studio using libmpv.

![screenshot](./screenshot.png)

Things left to fix/address

- The video seems to be darker (might be an issue with how obs handles hdr)
- Video filters do not work
- Audio control
    - MPV does not seem to offer any way of retrieving raw audio via a callback
        - On linux the source creates a jack audio capture and tells mpv to connect to it
          this works well and allows us to control and filter the audio in obs, however
          it does depend on jack or pipewire working. An option to choose beteween
          the different audio backends (oss, sndio, alsa, pulse, jack and pipewire)
          might be useful.
        - On windows there's only WASAPI (jack might also work) so there's no good way
          of controlling audio in obs unless we do some fuckery with the application specific audio capture
- The interact GUI works only for mouse movements, it does not react to clicks
