WMVSlop 0.22 - video wallpaper for the Xbox 360 dashboard (DashLaunch plugin)
===========================================================================

Plays one .wmv file from this folder on a loop behind the stock dashboard, with its sound.
For modified consoles (RGH, JTAG, or Bad Update with XeUnshackle) on dashboard 2.0.16197 to 2.0.17559.
Use at your own risk.

How to install
--------------
1. Copy this whole WMVSlop folder to the root of a FAT32 USB stick:  USB:\WMVSlop\
   Keep WMVSlop.xex and WMVSlop.ini together.
2. Put ONE .wmv video in the same folder. If there are several, the first by name is played.
3. In DashLaunch, set a plugin slot to  Usb:\WMVSlop\WMVSlop.xex  and save, or add under [Plugins] in
   launch.ini (use a free slot):
       plugin1 = Usb:\WMVSlop\WMVSlop.xex
4. Turn the console fully off and on. The video starts a few seconds after the dashboard appears.

Bad Update: put the folder on your exploit stick, add the plugin line to the launch.ini XeUnshackle
provides, and run the exploit. FreeMyXe cannot load plugins.

Videos
------
WMV video with WMA audio. 1280x720 at 30 fps plays best; 1080p at 60 fps stutters. The picture is
stretched to fill the screen, so use widescreen video. Try a new video on its own first: a file the
console's player can't handle can freeze the console.

If something goes wrong
-----------------------
- Pull the USB stick and restart. Nothing of the plugin is left on the console.
- After replacing WMVSlop.xex, turn the console fully off and on, or the old version keeps running.
- WMVSlop.log and WMVSlop-previous.log in this folder record what the plugin did. Send both when
  reporting a problem.
- Don't pull the stick while a video is playing; turn the console off first.

Settings are in WMVSlop.ini and are read once, at power-on.
