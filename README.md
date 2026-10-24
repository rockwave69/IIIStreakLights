III Streak Lights
====================

![Demo](screens/screen%20(1).png)

The plugin is built with Plugin-SDK. It keeps the game's normal lighting enabled and draws an additional screen-space effect.

How it works
============

- Projects each lamp position to screen space.
- Keeps a short history of six screen-space samples for each streak.
- Draws the streak with San Andreas' `CORONATYPE_HEADLIGHTLINE` texture and additive RenderWare blending.
- Uses normal coronas for the round light cores.
- Fades streak length and brightness according to camera angle and distance.
- Switches the rear streak from red to cool white-blue while reversing.
- Uses smaller reverse-light effects for motorcycles.
- Handles the two front lamps independently when a headlight is damaged.

This is not a direct port of re3's `STREAK_ON` implementation. San Andreas' Plugin-SDK corona API does not expose that same history mechanism, so the streak history and drawing are implemented separately.

Debug menu
==========

The plugin supports [aap/debugmenu](https://github.com/aap/debugmenu) for changing effect settings while the game is running.

The menu provides controls for:

- Front streak length, height, brightness and round core.
- Rear streak and side-view corona.
- Reverse-light streak and round core, including motorcycle scaling.
- Camera-angle and distance fade ranges.
- Resetting the settings to their defaults.

How to install
==============

1. Build the plugin as an ASI using a GTA San Andreas 1.0 US Plugin-SDK setup.
2. Copy the resulting ASI file to the game's root directory.
3. To use the in-game settings, place `debugmenu.dll` in the same directory. Do not rename the DLL.
4. Include `debugmenu_public.h` in the project when compiling the plugin with debug menu support.

The plugin can still run without `debugmenu.dll`; the in-game tuning menu will not be available.

Notes
=====

- The effects are experimental and may need tuning for individual vehicle models and visual setups.
- Debug menu changes are runtime values; this plugin does not currently save them between game sessions.
- The original GTA San Andreas lighting and stock vehicle lamps are not replaced by this plugin.
