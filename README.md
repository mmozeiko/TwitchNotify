# Twitch Notify

Simple utility that sits in system tray, monitors Twitch users and
notifies with Windows 10 toast notifications when they go live.

![screenshot][]

To add/change monitored Twitch users edit the `TwitchNotify.ini` file next
to `TwitchNotify.exe` executable. Put users inside `[users]` section each
on separate line. You don't need to restart application after editing file,
it will be automatically reloaded on change.

Alternatively you can download a list of your followed channels by entering
your Twitch username in the `.ini` file (ensure you removed the `;` before it
first), then either set `autoupdate` to `1` to automatically download the channels 
on startup or right-click the start bar icon and select "Download User List."

Note: Downloading followed user list will overwrite `[users]` section in `.ini` file.

Windows 10 notification toast allows to open Twitch user locally in [mpv][]
media player, or Twitch page in browser. You can access all monitored users
with right click on icon, it will show checkbox next to users that are
currently live.

To use [mpv][] player locally you need to have `mpv.exe` and either [yt-dlp.exe][]
(recommended) or [youtube-dl.exe][] executables available in PATH.

# Download

You can get latest build as zip archive here: [TwitchNotify.zip][]

# Building

* Install [Visual Studio 2022][]
* Run `build.cmd`

# License

This is free and unencumbered software released into the public domain.

Anyone is free to copy, modify, publish, use, compile, sell, or distribute this
software, either in source code form or as a compiled binary, for any purpose,
commercial or non-commercial, and by any means.

[screenshot]: https://raw.githubusercontent.com/wiki/mmozeiko/TwitchNotify/screenshot.png
[mpv]: https://mpv.io/
[yt-dlp.exe]: https://github.com/yt-dlp/yt-dlp
[youtube-dl.exe]: https://youtube-dl.org/
[TwitchNotify.zip]: https://raw.githubusercontent.com/wiki/mmozeiko/TwitchNotify/TwitchNotify.zip
[Visual Studio 2022]: https://visualstudio.microsoft.com/vs/
