# Twitch Notify

Simple Windows utility that sits in system tray and notifies you when Twitch users go live.

![screenshot](https://raw.githubusercontent.com/wiki/mmozeiko/TwitchNotify/screenshot.png)

To add/change monitored Twitch users edit the `TwitchNotify.txt` file next to `TwitchNotify.exe` executable. Put each username in separate line. You don't need to restart application after editing file, it will reload `TwitchNotify.txt` file automatically.

Click on baloon notifications to open stream for Twitch user. You can access all monitored users with right click on icon, it will show checkbox next to users that are currently live.

Live stream is opened either in livestreamer or browser. Offline users are always opened in browser.

To use [livestreamer](http://livestreamer.io/) you need to set up default quality and player in livestreamer configuration file `%APPDATA%\livestreamer\livestreamerrc`. As a minimum you want to have there following settings:

    player=mpv
    default-stream=best 

Change `mpv` to whatever player you use. Read [livestremer documentation](http://docs.livestreamer.io/cli.html#cli-options) about what other options you can put there. You can easily open notepad with this file from popup menu on tray icon.

# Download

You can get newest release as zip archive here: [TwitchNotify.zip](https://raw.githubusercontent.com/wiki/mmozeiko/TwitchNotify/TwitchNotify.zip)

# Building

* Install [Visual Studio 2015](https://www.visualstudio.com/en-us/products/vs-2015-product-editions.aspx)
* Run `build.cmd`
* TwitchNotify uses [jsmn](http://zserge.com/jsmn.html) JSON parsing library (MIT license)

# License

This is free and unencumbered software released into the public domain.

Anyone is free to copy, modify, publish, use, compile, sell, or distribute this software, either in source code form or as a compiled binary, for any purpose, commercial or non-commercial, and by any means.
