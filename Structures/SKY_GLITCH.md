# Sky glitch

Asked for and built on 2026-10-19, on branch `nature`. Not committed.

When a program hung on Windows XP and you dragged its window, the desktop behind it was never
repainted, so the window left a trail of copies of itself across the screen. Here the sky is the
desktop and the platforms are the windows: the sky is never wiped, so wherever a platform was drawn
against the sky, a copy of it stays there after the camera moves on. Fly around and the platforms
smear across the whole sky.

![Trails over the live sky, in play](Screenshots/sky_glitch_live_sky.png)

![Trails over a black sky](Screenshots/sky_glitch_black.png)

## What stays as it was

- **Lighting.** Only the final image changes, after tonemapping. The sky, sun, clouds and Lumen
  still render and light the platforms exactly as before.
- **The platforms.** Anything nearer than `wem.SkyGlitch.Distance` (1 km) always draws live, on top
  of the trails. The trails only ever fill what would be sky. The cloud floor counts as sky.
- **The editor viewport** outside of play. The glitch runs in game views: play and Simulate (a
  packaged game should work the same way but has not been tried).

## Options

As sliders: select **SkyGlitchSettings** in the Outliner, also while playing. Its Details panel
holds these settings, and since 2026-10-19 the portrait frame's too (its Portrait sections, see
`PORTRAIT.md`). It sets the console variables below when play begins and whenever a setting is
edited.

Console variables. Type them into the console (`~` in play, or the editor's Cmd box). They reset
when the editor restarts; to keep one, add it to `Config/DefaultEngine.ini` under `[SystemSettings]`,
for example `wem.SkyGlitch.Black=1`. A Blueprint can also set them with *Execute Console Command*.

| Console variable | Default | What it does |
|---|---|---|
| `wem.SkyGlitch` | 1 | 0 turns the glitch off. Turning it off and on again wipes the trails. |
| `wem.SkyGlitch.Black` | 0 | 1 draws the sky pure black, so the trails pile up on black instead of on the live sky. |
| `wem.SkyGlitch.ResetAfter` | 0 | Seconds the trails build up before they are wiped. 0 never wipes them. |
| `wem.SkyGlitch.TurnOnAfter` | 0 | Seconds the glitch stays off after a wipe, before it turns back on over a clean sky. 0 turns it straight back on. |
| `wem.SkyGlitch.Interval` | 0 | Seconds between the copies. 0 leaves one every frame. Larger values space the copies out into separate outlines. |
| `wem.SkyGlitch.Distance` | 100000 | In cm. Anything farther than this is sky. |
| `wem.SkyGlitch.Clear` | | A command. Wipes the trails and starts the ResetAfter clock over. |

With `ResetAfter 4` and `TurnOnAfter 3`, the cycle runs like this: on with trails for 4 s, then
wiped and off for 3 s, then on again over a clean sky. While the glitch is off, no trails are
drawn and the sky shows as normal, or stays black with `Black 1` (the user asked for this on
2026-10-20; before that, the off part showed the live sky even with `Black 1`). Play starts in the
"on" part of the cycle. The seconds are real time, so they keep counting while the game is paused.

![On, off, on again: captures at 1.7 s, 5.2 s and 8.0 s with ResetAfter 4 and TurnOnAfter 3, Black 1. Taken on 2026-10-19, when the off part still showed the live sky; it now stays black.](Screenshots/sky_glitch_cycle.png)

## With the AI overlay

The glitch stays on while the AI overlay (`AI_OVERLAY.md`) runs, and its trails carry the AI's look,
since the overlay is drawn first. (On 2026-10-20 the trails faded out as the AI ramped in; the user
had that removed the same day.)

## How it works

`Source/WEM_2026Rendering/Private/SkyGlitch.cpp` and `Shaders/Private/SkyGlitch.usf`.

A scene view extension adds its passes right after tonemapping. Each game view keeps a **trail
texture** the size of its view. In it, alpha 1 means painted and 0 means never painted.

1. **Stamp.** Platform pixels take the platform as it is drawn now. Sky pixels keep whatever was
   painted there before.
2. **Composite.** Platform pixels show the live frame. Sky pixels show the trail where one was
   painted, and the live sky (or black) everywhere else.

What counts as a platform is decided by depth. A pixel counts as a platform when it or any of
its four neighbours is nearer than `Distance`, so platform edges stay live instead of
flickering. The copies stay fixed on the screen, like the XP desktop, and do not turn with the
camera.

Shaders need a module that loads before the engine gathers its shader types, so this is a module
of its own, `WEM_2026Rendering`, loading at `PostConfigInit` (see `WEM_2026.uproject`). It maps
the project's `Shaders/` folder to `/WEM_2026`.

## Cost

Measured with `ProfileGPU` in Simulate at 2713×1827: **Stamp 0.029 ms + Composite 0.021 ms**, in
a 12.4 ms frame. While the glitch is off, only the composite runs, to draw the black sky with `Black 1`; without it, no pass runs.

Memory: one RGBA16F texture per view. That is about 40 MB at 2713×1827, and two exist while one
frame paints over the last.

## Things to know

- The editor's Plugins window rewrites `WEM_2026.uproject`. If it ever drops the
  `WEM_2026Rendering` entry, the glitch quietly stops loading.
- Split screen: each player's view keeps its own trail.
- At 0 Interval the copies are as far apart as the camera moved in one frame. A fast turn gives
  separate stacked copies, like the XP picture. A slow drift gives a near-solid smear.
