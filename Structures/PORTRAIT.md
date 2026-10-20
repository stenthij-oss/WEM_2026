# Portrait frame and phone camera

Asked for and built on 2026-10-19, on branch `nature`. Not committed.

Portrait mode, like a phone held upright and filming: the game draws in a tall, narrow frame in
the middle of the window, with black bars at the sides, and looks like footage from a phone. The
picture is never turned or stretched.

![The portrait frame with the phone look in play, over the sky glitch's black sky](Screenshots/portrait_frame.png)

## Switching it on and off

- **P** while playing toggles between portrait and the normal camera. Everything below comes and
  goes with it.

## Sliders

Select **SkyGlitchSettings** in the Outliner. Its Details panel has a **Portrait** section (frame
ratio, focal length scale) and a **Portrait › Phone Look** section (phone look, sharpen, colour
fringes, hand shake, rolling shutter), below the sky glitch's own settings.

- Edited while not playing, the values are saved with the level and applied each time play
  begins.
- Edited while playing (select the actor in the Outliner during play), they apply at once but
  are lost when play stops, like any change in play.
- Portrait on and off stays on P. Editing a slider never switches the frame off.

## Console

The same settings as console variables (`~` while playing, or the editor's Cmd box). The actor's
values replace these whenever play begins or a slider is edited.

| Console | Default | What it does |
|---|---|---|
| `wem.Portrait` | 0 | 1 is the portrait frame with the phone look, 0 the normal camera. |
| `wem.Portrait.Toggle` | | A command. Switches between the two, the same as P. |
| `wem.Portrait.Ratio` | 0.5625 | Width of the frame for a height of 1. 0.5625 is 9:16. A taller phone, 9:19.5, is 0.4615. |
| `wem.Portrait.FocalLengthScale` | 1 | Multiplies the lens's focal length. Below 1 is wider, above 1 narrower. |
| `wem.Portrait.Effects` | 1 | How strong the phone look is. 0 is a clean camera, 2 goes twice as far. |
| `wem.Portrait.Sharpen` | 1 | How hard edges are sharpened (the bright and dark halos), on top of Effects. 0 none, 2 twice as much. |
| `wem.Portrait.Fringe` | 1 | How far the red and blue colour fringes spread, on top of Effects. 0 none, 2 twice as far. |
| `wem.Portrait.Shake` | 1 | How much the hand holding the phone moves. 0 holds it still. |
| `wem.Portrait.RollingShutter` | 0.08 | Seconds the sensor takes to read, top to bottom. 0 turns the lean off. |

All of these work live while playing. Typed values reset when the editor restarts; the sliders
are the way to keep them. Every editor start begins with the normal camera. To start in portrait
every time, add `wem.Portrait=1` to `Config/DefaultEngine.ini` under `[SystemSettings]`.

The sharpening and the fringes were toned down on 2026-10-19 (the user found them a bit too
strong): sharpening from 1.4 plus the engine's own 0.6 to 0.8 alone, fringes from 0.6 % to 0.35 %
of the distance from the centre.

## The lens

The frame doesn't crop the picture. It turns the lens upright, like turning a phone: the lens's
wide side now runs from the top of the frame to the bottom. The drone starts on its 8 mm lens,
which is then 112° tall and 80° across. That's as wide as a phone's 0.5× ultra-wide camera. The
first version kept the picture's height and cut the sides away, which only left 90° × 59°.
The zoom keeps working on top: `=` / `-` and holding A.

## The phone look

Exaggerated on purpose:

- **"HDR" processing:** highlights and shadows pressed together into a flat, evenly bright
  picture, with fine detail pushed up until it crunches. Punchy colour, a little extra contrast.
- **Over-sharpening:** bright and dark halos along edges.
- **Sensor noise:** blotchy brightness and colour specks, strongest in the shadows and in the
  darkened corners.
- **The small lens:** red and blue colour fringes towards the edges, a round vignette and glowing
  lights (bloom). No lens flare ghosts: the engine's ghost blobs are switched off in portrait,
  because they sat in the middle of the frame and distracted.
- **Phone camera behaviour:** quick auto exposure that pumps between dark and bright, little
  motion blur, and everything in focus (no depth of field).
- **The hand:** a slow sway with a little tremor.
- **Rolling shutter:** turning leans uprights, tilting stretches or squashes the picture. The
  stabiliser renders the frame a little wider and crops it back, so the lean never shows an
  edge.

Every effect is measured inside the portrait frame, from its own centre and edges, never from the
whole window.

## Where it applies

Game and play-in-editor views, with any camera. The editor viewport outside of play and Simulate
are left alone, because they have no player. The sky glitch follows the frame; switching wipes
its trails, because the frame changes size.

## How it works

`Source/WEM_2026Rendering/Private/PortraitFrame.cpp`, `PhoneCamera.cpp` and
`Shaders/Private/PhoneCamera.usf`.

- A scene view extension changes each player's camera before the engine shapes the picture. It
  turns on the camera's *Constrain Aspect Ratio* with the portrait ratio and sets the field of
  view for the upright lens, adds the hand's sway, and sets the phone's post process values as
  camera overrides. The engine then draws the frame, the black bars and its effects inside the
  frame.
- A pass after tonemapping does what the engine has no setting for: the stabiliser's crop, the
  rolling shutter, colour fringes, over-sharpening, the vignette and the noise. It runs after the
  sky glitch, so the phone films the glitched sky as well. The fringes and the vignette are done
  here and not with the engine's settings for a reason: the engine draws those before the sky
  glitch, which then painted them into its trails as rainbow streaks.
- Nothing runs while portrait is off.

P is a debug key binding (`Config/DefaultInput.ini`, `[/Script/Engine.PlayerInput]`). It works
in the editor and in development builds, not in a shipping build. The console variables work
everywhere.

Tests (`WEM.Portrait.*`): `Fit` checks the upright lens and the frame's ratio through the
engine's own projection code. `PhoneLook` checks the post process values, the stabiliser's
margin and the hand. `RollingShutter` checks the turn rate and which way the picture leans.
`Toggle` checks the toggle.
