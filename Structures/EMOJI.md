# Emoji

Asked for and built on 2026-10-19/20, on branch `nature`. Not committed.

Press the **right stick in** and hold it: a small ring of nine emoji opens in the middle of the
frame. Lean the stick towards one and it grows a little, to show it's the one you're picking. Turn
the stick round to pick another. Let go of the stick button and two things happen:

1. **On your screen**, a stream of that emoji rises from the bottom of the frame, each one swaying
   and leaning as it goes, growing in as it surfaces and fading out near the top.
2. **In the world**, the drone gives off a short burst of that emoji on little squares that always
   face the viewer. They're big at the drone and get smaller the farther from it they are, and
   they're gone within a second and a half. The other player sees these too.

![The menu held open, ThumbsUp picked](Screenshots/emoji_menu.png)

![Just after letting go: the burst flying out from the drone](Screenshots/emoji_burst.png)

![Half a second later: the burst shrinking away, the screen emoji rising](Screenshots/emoji_sent.png)

## Controls

- **Right stick, pressed in and held**: the menu stays open.
- **Right stick, leaned**: picks the emoji it points at (the first is straight up, then clockwise).
  While the menu is open, the stick doesn't turn the drone. After you let go, it turns the drone
  again once it's back near the middle, or after 0.6 s, so the view doesn't swing as your thumb
  comes off.
- A pick holds while the stick springs back to the middle, so let go of the button whenever.
- Letting go with nothing picked sends nothing.
- LB/RB still turn the drone while the menu is open.

## The emoji

`Content/Emoji/T_Emoji_*`, made from Windows' own colour emoji font (Segoe UI Emoji), so nothing was
downloaded. In order round the ring, clockwise from the top:

😀 Grin · 😂 Joy · 😍 HeartEyes · 😮 Wow · 😢 Cry · 😡 Angry · 👍 ThumbsUp · ❤️ Heart · 🔥 Fire

To change them, either:

- put other textures in the component's **Emoji** list (any number works; the ring spreads them
  evenly), or
- edit the list in `Tools/Emoji/render_emoji.py`, run it
  (`python Tools/Emoji/render_emoji.py`, needs Pillow), and import the PNGs it writes to
  `Tools/Emoji/png/` into `/Game/Emoji`.

A texture used here should have Never Stream on (the imported ones do), so the menu never shows it
blurry.

## Settings

Open `BP_OpalHoverDrone` and select its **Emoji** component (under CineCamera). To change one playing
drone only, select the drone in the Outliner while playing. Changes to a playing drone show at once.

| Setting | Default | What it does |
|---|---|---|
| Emoji | the nine above | The emoji to choose from, round the ring clockwise from the top. |
| **Input** | | |
| Menu Key | Gamepad Right Thumbstick Button | Held down, keeps the menu open. |
| Aim Key | Gamepad Right Thumbstick 2D-Axis | Picks while the menu is open. It stops turning the drone meanwhile. |
| Stick Up Is Negative | on | Whether the stick pushed up reads as a negative lean. It does on the controllers here, and it's also what tilts the drone's view up. Turn it off for a controller that reads up as positive. |
| Aim Dead Zone | 0.5 | How far the stick has to lean, 0 to 1, before it picks. |
| **Menu** | | |
| Menu Radius | 0.12 | How far the emoji sit from the ring's middle, as a share of the frame's height. |
| Menu Icon Size | 0.065 | How big each emoji in the ring is, as a share of the frame's height. |
| Picked Scale | 1.45 | How much bigger the picked emoji grows. |
| Menu Open / Close Seconds | 0.16 / 0.12 | How long the ring takes to open and to shut. |
| **Screen** | | |
| Screen Count | 14 | How many rise over the screen for one sent. |
| Screen Spread Seconds | 0.8 | The time over which they set off, one after another. |
| Screen Seconds | 2.6 | How long each takes to rise from the bottom to the top, give or take a fifth. |
| Screen Size | 0.085 | How big they are, as a share of the frame's height, give or take a quarter. |
| **Drone** | | |
| Burst Count | 30 | How many the drone gives off for one sent. |
| Emit Seconds | 0.4 | How long it keeps giving them off. The first third come at once. |
| Speed | 650 cm/s | How fast they fly out, at most. The slowest go at 60% of it. |
| Drag | 1.2 | How quickly the air slows them. |
| Forward Bias | 0.5 | 0 sends them out every way alike. Towards 1, more go the way the camera faces (at 1, all within 30°). |
| Inherit Velocity | 0.5 | How much of the drone's own speed they set off with. At 0, a fast drone leaves them straight behind. |
| Lifetime | 1.5 s | How long each lasts at most. Some go a quarter sooner. |
| Near Size | 40 cm | How wide one is right at the drone. |
| Far Size | 3 cm | How wide one is once Shrink Distance or farther from the drone. |
| Shrink Distance | 700 cm | How far from the drone they've shrunk all the way. They shrink fastest near the drone. |
| Quad Mesh, Material (Advanced) | engine Plane, `M_Emoji` | The square and its look. The material takes the texture parameter `Emoji` and the scalar `Brightness`. |

The burst comes out where the component sits: 60 cm in front of the camera and 20 cm below it. Move
the component to move where it comes out.

Blueprint: **Send Emoji** (index) sends one as letting go of the menu would. **Is Menu Open** says
whether the menu is held open.

## How it works

- `UEmojiComponent` (on the drone) makes its own input at Begin Play: an action for the menu key in
  a mapping context added to the controlling player, and an action for the stick in a second
  context. That second context is added only while the menu needs the stick. It sits above the
  drone's own input, so Enhanced Input gives the stick to it and not to turning. `IMC_Drone` and
  `IM_HoverDrone` are untouched.
- The menu and the rising emoji are one Slate widget (`SEmojiOverlay`) on the controlling player's
  part of the screen. It's drawn over the finished picture, so the sky glitch, the phone look and
  the AI overlay never touch it. In portrait mode (P) it sits inside the portrait frame.
- The burst is drawn with one instanced mesh per emoji sent: squares in world space, turned each
  frame to face the player's camera with its up as their up, plus a little turn of their own. The
  material (`M_Emoji`) is unlit and masked, with exposure undone (like the drone trail), so the
  emoji keep their colours in bright and dark places. Masked rather than translucent, so they
  write depth: the sky glitch counts them as platforms and draws them live, rather than smearing
  them across the sky. They cast no shadow and are left out of Lumen and ray tracing.
- Size at a distance `d` from the drone: `lerp(NearSize, FarSize, 1 - (1 - d/ShrinkDistance)²)`.
  Each also grows in over its first 0.08 s and shrinks away over the last quarter of its life.
- At most 600 burst emoji are in flight at once, so a quick run of sends can't pile up.

## Testing aids

- `wem.Emoji.Send <index>` (console, while playing): sends that emoji from every playing drone, as
  letting go of the menu would.
- `Input.+key Gamepad_RightThumbstick`, then `Input.+key Gamepad_Right2D X=0.64 Y=-0.77` (leans to
  the second emoji: with Stick Up Is Negative on, a negative Y is up), then `Input.-key` both:
  drives the real menu input from the console.
- Tests: `WEM.Emoji.Pick`, `WEM.Emoji.Menu`, `WEM.Emoji.Burst`, `WEM.Emoji.Screen`.

## Status

- The tests pass (`WEM.Emoji.*`, 4 of 4, 2026-10-20).
- Checked in play on 2026-10-20 (the screenshots above):
  - The ring opens, the leaned-to emoji grows and lights up, and the rest dim.
  - Letting go sends the burst, with the squares upright and not mirrored, and the emoji rise
    over the screen.
  - The stick stops turning the drone while the menu is open. With the menu shut, the stick held
    right turned the drone 42° in 0.6 s. With the menu open and the stick held hard left for
    0.8 s, the drone turned 0.3°, the tail of the turn before. It didn't swing after letting go.
- On the user's controller, the stick pushed up picked the bottom emoji (2026-10-20). The
  controllers here read up as a negative Y, which is also what tilts the drone's view up (the
  drone pitches by -Y). The menu took a positive Y as up, and so did the console test above, so it
  looked right there. Fixed with Stick Up Is Negative, on by default. The user tried it on the
  real controller: up picks the top emoji, and the other directions match.
- Fixed along the way:
  - The engine plane carries the picture's top along its -Y, which showed the emoji turned a
    quarter. The squares now get a quarter turn about their face.
  - Near Size went from 60 to 40 cm. At 60, the first moments of a burst covered most of the
    sender's own view.
  - Each square's own turn is now at most ±15° to start and ±35°/s. It was ±25° and ±90°/s,
    which left some thumbs pointing sideways within a second. This one is built into the editor
    but hasn't been looked at in play.

## Limits

- Only the gamepad opens the menu. There's no keyboard or mouse way yet.
- Facing follows the first player's camera (or the controlling player's). In split screen, the
  other player would see the squares turned towards the first.
