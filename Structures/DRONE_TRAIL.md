# Drone trail

Asked for and built on 2026-10-19, on branch `nature`. Not committed.

Wherever the drone flies it leaves a thin, glowing tube of light behind it: its pathway through
the cube. The trail stays for the whole game. Nothing fades or is cleared, because it costs too
little to need that (see *Cost* below).

![The trail behind a half circle flown outside the cube; the thin line is a teleport during testing](Screenshots/drone_trail_circle.png)

![Eight hours of made-up flight laid into an 80 m box with wem.DroneTrail.Prefill](Screenshots/drone_trail_8_hours.png)

## Settings

On the drone: open `BP_OpalHoverDrone`, select its **Trail** component. To change it for one
playing drone only, select the drone in the Outliner while playing. Changes while playing reach
the whole trail, the part already flown included.

| Setting | Default | What it does |
|---|---|---|
| Color | blue (0.05, 0.45, 1.0) | The trail's colour. The brightest middle of the tube goes towards white. |
| Glow | 10 | How brightly it shines. 1 is about as bright as a white wall in the current light, whatever the exposure; more glows and blooms. |
| Width | 6 cm | How thick the tube is. Far away it never gets thinner than a pixel or two, so a distant trail stays an unbroken line. |
| Accuracy (Advanced) | 1 cm | How far the drawn trail may stray from where the drone really flew. Smaller follows tight turns more closely, with more points. |
| Material (Advanced) | `/Game/DroneTrail/M_DroneTrail` | Takes the parameters `Color`, `Glow` and `Radius` (cm). Its own `Core` (0.6) sets how white the middle of the tube burns, `MinPixels` (1.5) how thin a far trail may get. |

The trail comes out where the Trail component sits: 25 cm under the camera, so looking back
you see it stream away from under you. Move the component to move where it comes out.

Blueprint: **Set Color**, **Set Glow** and **Set Width** on the Trail component do the same
while playing.

## How it works

- `UDroneTrailComponent` (on the drone) watches where it is every frame. A new point is added
  only when a straight line from the last point would pass more than *Accuracy* from where the
  drone really was (`FDroneTrailPath`). Straight flight adds no points, tight turns add many.
  The two-hour test flight below averaged 6.4 points a second.
- The tube runs through those points, 8 sides, with rounded ends at the start and at the drone.
  It is cut into pieces of 1024 rings (`UDroneTrailMeshComponent`, its own scene proxy). Only the
  newest piece changes, a few rings a frame, uploaded in place. A full piece is finished and
  never touched again: one cached draw, and its vertices live only on the GPU.
- The material (`M_DroneTrail`) is unlit and opaque: emissive colour times Glow, with exposure
  undone (EyeAdaptationInverse), so it shines the same in bright and dark scenes. The edges of
  the tube are dimmer than its middle, and the middle burns towards white, so it reads as a round
  light. Far away the material fattens the tube to at least 1.5 pixels, so it doesn't break up
  into dashes.
- Changing a setting while playing makes Unreal rebuild the drone's components. The new Trail
  component takes the trail over (`FDroneTrailInstanceData`), so nothing is lost.
- The trail casts no shadow and is left out of Lumen, distance fields, ray tracing and the
  velocity pass. It lights nothing around it: the glow is bloom.
- Opaque means the trail counts as a platform for the sky glitch: it always draws live, and leaves
  copies across the sky like the platforms do.
- The AI overlay never paints the trail, the same way it never paints the orbs. The overlay paints
  only where custom stencil holds the pieces' value (`wem.AIOverlay.Stencil`, 1). The trail writes
  custom depth with a value of its own (3; the orbs use 2), so where it passes in front of a piece
  that piece's value is covered.

## Cost

Measured on 2026-10-19 (RTX 4090, 2713x1827 PIE, TSR):

- **GPU, 8 hours of trail on screen** (182 pieces, `wem.DroneTrail.Prefill 480`, all around the
  camera): PrePass 0.09 → 0.11 ms and BasePass 0.80 → 0.94 ms, plus 0.24 ms in the velocity pass,
  so about 0.4 ms in all. The velocity pass has been turned off for the trail since. A later
  profile showed it gone, but the GPU was shared with another job then, so the new total was not
  re-timed. Since then the trail is also drawn into the custom depth pass, to stay out of the AI
  overlay, and that has not been timed either. It should cost about what the velocity pass did,
  so expect roughly 0.4 ms for eight hours of trail on screen.
- **Memory**: about 0.2 MB of vertices on the GPU per piece; 8.6 MB after two hours of flying,
  about 35 MB after eight. Under 2 MB of points on the CPU after two hours.
- **CPU, while flying in play** (`stat dumpframe`): about 0.005 ms on the game thread and 0.01 ms
  on the render thread a frame. `stat DroneTrail` shows both. In
  `WEM.DroneTrail.Perf.LongFlight` (two hours of wandering flight at 60 fps, no rendering) the
  worst frame was 0.19 ms: the frame a piece is finished.

## Testing aids

- `wem.DroneTrail.Prefill <minutes>` (console, while playing): lays that many minutes of made-up
  flight into every drone's trail, wandering within 40 m of the drone. 480 shows what eight hours
  look like and cost, and takes under 2 seconds.
- `stat DroneTrail`: the trail's time a frame.
- Tests: `WEM.DroneTrail.Path`, `WEM.DroneTrail.Pieces`, `WEM.DroneTrail.Prefill`, and the slow
  `WEM.DroneTrail.Perf.LongFlight`.

## Limits

- The trail is not kept between plays.
- A drone that jumps (teleport) draws a straight line from where it was.
- The drone has no visible body, so the other player sees the trail come out of thin air.
