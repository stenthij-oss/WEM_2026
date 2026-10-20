# AI overlay

Asked for and built on 2026-10-19, on branch `nature`. Not committed.

Every 20 to 30 seconds a locally run AI takes over the platform pieces on screen. It ramps in, so
the pieces drift further from how they really look, holds at full for a few seconds, and ramps out
again. Only the pieces change: the sky, the clouds and everything else stay as they are. The AI
sees only the pieces too: each frame it gets the pieces on screen, with everything else black.

The AI is StreamDiffusion running SD-Turbo, one denoising step per picture, on the same RTX 4090
as the game. It runs in its own Python process, the **bridge**, next to the editor or the game.
Since 2026-10-19 the game also sends the pieces' depth with each picture, and a depth ControlNet
holds the AI to their shape (see *Depth*, below). Since 2026-10-20 the AI makes a picture from
every frame it can rather than 15 a second, and each picture is laid where the pieces are in the
frame being drawn, fading into the next (see *Smoothness*, below).

![At full, in play (portrait frame on), with the bridge's placeholder prompt: the pieces turn to weathered concrete; the blue lattice and the sky stay live](Screenshots/ai_overlay_full.png)

The bridge's placeholder prompt, until a real one is chosen: *weathered prefabricated concrete
panel building, East German Plattenbau, photograph, overcast light*.

## Running it

1. Once: fetch the model and the depth ControlNet (about 3.3 GB together) into
   `Saved/AIOverlay/models`. It resumes if the connection drops; run it again until it says
   `Done`. Finished files are skipped, so after an update it fetches only what is new.

   ```
   StreamDiffusion\.venv\Scripts\python.exe Tools\AIOverlay\download_model.py
   ```

2. Start the bridge and leave it running. It takes about a minute to load and then prints
   `Ready`. Between cycles it sleeps. With the depth ControlNet, its TensorRT engines are its
   own (in `Saved/AIOverlay/engines-depth`): the first start builds them, about 20 minutes.

   ```
   Tools\AIOverlay\run_bridge.bat
   Tools\AIOverlay\run_bridge.bat --prompt "what to turn the pieces into"
   ```

3. Play. The first cycle comes 20 to 30 seconds after play starts. Without the bridge running,
   nothing changes.

The editor and the bridge can start in either order, and the bridge can be stopped (Ctrl+C) and
started again while the game runs.

A packaged game starts the bridge itself, without a window, and stops it when it quits; only one
bridge runs at a time. `Structures/BUILD.md` has the build and how to run the bridge by hand for
it.

## Options

**The menu.** The **AI Overlay** button on the level editor's toolbar has two entries:
**Settings...** opens the overlay's page in Project Settings (Game > AI Overlay), and **Start a
Cycle Now** brings the AI in at once during play. The page has the timing (on/off, shortest and
longest gap, ramp up, hold, ramp down, constant), the look (prompt, strength, opacity, clip to
the pieces, segments taken over, show), the depth (on, weight, fit to the view, near, far), the
smoothness (most pictures a second, keep on the pieces, blend between pictures) and two
experimental settings, off by default (picture size, hold the game to the AI's rate). A
change applies at once, also in the middle of play, and is saved with the project in
`Config/DefaultGame.ini`, so it holds after a restart and in a game build too.

The page's prompt starts as the bridge's placeholder, and whatever is in it wins over the
bridge's `--prompt`. Empty it to use the bridge's own.

Each setting drives one console variable, below (`UAIOverlaySettings`,
`Source/WEM_2026Rendering/Private/AIOverlaySettings.h`). Typing one into the console (`~` in
play, or the editor's Cmd box) still works until the page sets it again. A typed value is lost
when the editor restarts; the page's values come back.

| Console variable | Default | What it does |
|---|---|---|
| `wem.AIOverlay` | 1 | 0 turns the overlay off. |
| `wem.AIOverlay.Start` | | A command. Starts a cycle now instead of waiting for the gap to end. |
| `wem.AIOverlay.GapMin` | 20 | Shortest time, in seconds, the picture stays normal between two cycles. |
| `wem.AIOverlay.GapMax` | 30 | Longest gap. Each gap is picked at random between the two. |
| `wem.AIOverlay.RampUp` | 8 | Seconds the AI takes to come in fully. |
| `wem.AIOverlay.Hold` | 5 | Seconds it stays fully in. |
| `wem.AIOverlay.RampDown` | 8 | Seconds it takes to go again. |
| `wem.AIOverlay.Constant` | 0 | 1 keeps the AI in: it ramps in once, with no gap first, and stays at full. Set back to 0, it holds, ramps out, and the gaps start again. Turned on while the AI ramps out, it turns back up from where it is. |
| `wem.AIOverlay.Opacity` | 1 | How much of the AI's picture covers the pieces at full. 1 covers them completely. |
| `wem.AIOverlay.Strength` | 0.6 | How far the AI may move from the pieces at full. 0 hands them back as they are, 1 lets it paint whatever the prompt says. |
| `wem.AIOverlay.Prompt` | the placeholder (from the page) | What the AI turns the pieces into. Empty keeps the bridge's own prompt (its `--prompt`). Put it in quotes. |
| `wem.AIOverlay.ClipToMask` | 1 | The AI's picture comes back a few frames late. 1 cuts it to where the pieces were when it was sent (with `Reproject`, to the spots its view saw), so it does not smear past them while the camera moves. 0 lets it trail. |
| `wem.AIOverlay.SegmentPercent` | 100 | What share of the segments, in percent, the AI takes over; the others stay as they are. See *Segments taken over*, below. |
| `wem.AIOverlay.Stencil` | 1 | The custom stencil value that marks the pieces. Only change it together with `AIOverlayStencil` in `RoomManager.cpp`. |
| `wem.AIOverlay.Depth` | 1 | Sends the pieces' depth with each picture, when the bridge takes it (see *Depth*). 0 sends none, and the bridge leaves its ControlNet out. |
| `wem.AIOverlay.DepthWeight` | 0.7 | How strongly the depth holds the AI to the pieces' shape: the ControlNet's weight. 0 leaves the AI to the picture alone; above 1 holds harder still. |
| `wem.AIOverlay.DepthFit` | 1 | Fits the depth map to the view: the nearest pieces on screen white, the furthest black. 0 runs it from `DepthNear` to `DepthFar` instead. |
| `wem.AIOverlay.DepthNear` | 0.5 | In metres: the nearest the depth map's range goes; with `DepthFit` 0, what is white. |
| `wem.AIOverlay.DepthFar` | 200 | In metres: the furthest the range goes; with `DepthFit` 0, what is black. |
| `wem.AIOverlay.Show` | 0 | What the overlay draws: 0 the AI's picture over the pieces, 1 the picture the AI is sent, 2 the depth map it is sent, both over the whole view. Not saved. |
| `wem.AIOverlay.MaxRate` | 0 | At most this many pictures a second from the AI. 0 lets it make one from every frame it can (unless the bridge was started with a `--max-rate` of its own). 15 is how it ran until 2026-10-20. See *Smoothness*. |
| `wem.AIOverlay.Reproject` | 1 | Lays each of the AI's pictures where the pieces it shows are in the frame being drawn, by their depth. 0 lays it where it was on screen when it was sent, as before 2026-10-20. |
| `wem.AIOverlay.Blend` | 1 | Fades each new picture in over the one before (over about the time between two, at most a fifth of a second). 0 switches at once. |
| `wem.AIOverlay.Size` | 0 | Experimental. The side, in pixels, of the pictures the AI works at, 384 to 1024 in steps of 64; 0 keeps the bridge's own (512). The bridge loads its model again at the new size, a few seconds without the AI. |
| `wem.AIOverlay.MatchFrameRate` | 0 | Experimental. 1 holds the game's frame rate (`t.MaxFPS`) to just above the AI's pictures a second while they trade pictures, and puts it back after. |

Both the cover and the strength follow the cycle: at the start of a ramp the AI barely changes the
pieces and barely shows; at full, it shows `Opacity` of a picture made at `Strength`. The seconds
are real time, so they keep counting while the game is paused. A new play session starts with a
fresh gap, or, constant, ramps straight in.

Constant keeps the bridge working the whole time, so the game loses its share of frames (see
Limits) the whole time too, not only during cycles.

**Segments taken over** (`SegmentPercent`). Below 100, only that share of the segments draws the
mask, and the AI only ever covers those; the rest of the structure stays as it is, also at the
height of a cycle. Each segment is picked by itself, the fill on both its faces together, and it
comes with the beams and posts round its rim, so a picked panel is taken over with its frame. A
beam or post is shared between segments, so it goes in when any segment it frames is picked: the
edges of an unpicked segment beside a picked one go in with it. A beam running across the inside
of a segment rather than round it counts only for the segments it is rim to. A segment's pick
depends only on where it is, so it keeps its pick while the structure grows and shrinks around it,
every play session picks the same ones, and a higher share keeps every segment a lower one picked
(30 % is the 20 % plus more). As segments come and go, the beams and posts round them follow. The AI is sent only the picked pieces, with
everything else black, as it was sent all the pieces before.

Set before play, it costs nothing. Changed during play, every piece that changes sides is taken
out and drawn again (a chunk of pieces has one stencil value, so the picked and the other pieces
are kept in chunks of their own), and a big change means a hitch while that happens. On the
settings page the slider applies when it is let go, not while it is dragged.

**Depth.** SD-Turbo on its own only sees the colours: at a low strength it keeps the pieces but
hardly changes them, at a high one it repaints them, but it loses the lattice too and paints
whatever building the prompt suggests. With the depth, a ControlNet (an SD 2.1 depth ControlNet,
since SD-Turbo is SD 2.1 underneath) steers each picture to keep what is near and what is far,
the planes and their edges, so the strength can go much higher and the structure still holds.

The depth map is grey: the pieces near the camera white, the far ones dark, everything that is
not a piece black. Fitted to the view (the default), the nearest pieces on screen are white and
the furthest black, as in the depth maps the ControlNet learned from; the range eases after the
camera over a fraction of a second, so the map does not pulse as pieces come into view. Without
the fit, from far away nearly everything is dark, and the ControlNet then reads it all as far
off and holds the shape only weakly. Between near and far, even steps of grey are even ratios of
distance, so near pieces keep their detail and far ones still differ.

**Show** draws what the AI is sent instead of what it makes, over the whole view: the picture,
or the depth map. It draws all the time, between cycles and without the bridge too, and after
the sky glitch and the phone camera, so neither paints over it. It is not saved: after a restart
the overlay is back to the AI's picture.

The bridge's own options, given when it starts (not in the menu): `--prompt`, `--negative` (has
no effect for now: the bridge runs SD-Turbo without guidance, and StreamDiffusion then ignores
the negative prompt), `--seed`, `--width` and
`--height` (the size the AI works at, 512 x 512 by default, multiples of 64, unless the game asks
for another), `--acceleration` (`none` or `tensorrt`), `--model` (a Hugging Face id or a local
folder), `--max-rate` (at most this many pictures a second when the game sets no cap of its own;
0, the default since 2026-10-20, = one from every picture the game sends; it was 15), `--depth`
(the depth ControlNet's folder, `Saved/AIOverlay/models/controlnet-depth` by default),
`--no-depth` (without it: faster, and less GPU memory), `--sync-steps` (waits for the GPU after
each step of a picture, as StreamDiffusion does on its own; a little slower, for comparing) and
`--channel` (see below). The bridge prints every 5 seconds of a cycle how many pictures it made,
how long each took and how much of the time it was busy (the rest it waited for pictures). The game
sends a new picture every frame, so without a cap the bridge works flat out during a cycle; with
one it sleeps between pictures and always takes the newest.

**Channels.** A game started with `-AIOverlayChannel=N` on its command line trades pictures only
with a bridge started with `--channel N`; plain games and the editor use channel 0, as before.
Every game writes to the same shared memory otherwise, so a second game (a measurement run in
the background, say) would take the pictures of the one being played. The channel is added to
the second group of digits of the blocks' GUID names, since Windows lets two programs share a
block without administrator rights only under a bare GUID.

## Smoothness

Asked for on 2026-10-20: the AI should change about as often as the game draws a frame, and
look smooth. Until then the bridge made at most 15 pictures a second while the game drew 60 to
100, so the pieces changed in steps every four to six frames, and each picture came back a few
frames late, so while the camera moved it lagged behind the pieces (or, clipped to them, was cut
short). Three things changed, all on by default, and two more can be tried:

- **One picture from every frame the AI can manage** (*Most Pictures a Second* = 0). The game
  already sent a picture every frame; the bridge now takes the newest one as soon as it is free
  instead of waiting for its 15-a-second turn. The bridge itself also got faster: the pictures go
  straight from shared memory to the GPU and back through page-locked buffers, the model's steps
  are called directly instead of through StreamDiffusion's wrapper (its checks, conversions and
  waits), and the depth ControlNet is replayed as one CUDA graph, as StreamDiffusion already did
  for the UNet. Same pictures, bit for bit; on its own, with nothing else on the GPU, the bridge
  went from 55 to 65 pictures a second at 512 with depth (the UNet takes about 8 ms of that, the
  ControlNet 5, the tiny autoencoder 2). StreamDiffusion also waits for the GPU after each of
  its engines; the bridge now runs everything of a picture on the engines' own CUDA stream, which
  keeps the steps in order without waiting, and waits once, at the end: 70 pictures a second
  alone (`--sync-steps` puts the waits back). Beside the game that helped less than hoped (18 to
  19 pictures a second): there a picture of 14 ms takes 24 to 32 ms, however its work is sent,
  since the GPU switches between the game and the bridge (raising the bridge's GPU scheduling
  priority changed nothing either, tried the same night).
- **Keep on the Pieces** (`Reproject`). Each picture is laid where the pieces it shows are in the
  frame being drawn: when a picture is sent, the game keeps where the camera was and how far the
  pieces lay under each of its pixels; when the AI's answer comes back, each pixel of the pieces
  on screen is taken back through the camera of that moment to where that picture saw it. A spot
  the picture did not see (hidden behind something nearer then, or off its edge) takes the
  picture before it instead, or, if that did not see it either, stays as it is. So a picture a
  few frames old still sits on the pieces at the game's full frame rate, also while the camera
  turns.
- **Blend Between Pictures** (`Blend`). Each new picture fades in over the one before, over about
  three quarters of the time between two of them (at most a fifth of a second), so the pieces
  flow from one picture to the next rather than jump, and the AI's frame-to-frame "boiling" is
  softened. The fade costs a little freshness: a new picture is fully in a moment after it comes.

![The same turn of the camera, with a stand-in for the AI that hands each picture back unchanged, 50 ms late, 20 times a second: on the left as before, the picture lags the turn and leaves black edges; on the right it sits where the pieces are](Screenshots/ai_overlay_reproject.png)

Experimental, off by default:

- **Picture Size** (`Size`). The AI works at 512 x 512 unless asked otherwise; smaller is faster
  and coarser. The TensorRT engines take any size from 384 to 1024, so a new size needs no new
  engines, but the bridge loads its model again, which takes about 8 seconds without the AI.
  Smaller pictures help less than one would think: at 384 the bridge alone makes 66 to 75
  pictures a second against 55 to 65 at 512, since at one picture at a time the GPU is held up by
  the number of steps more than by their size.
- **Hold the Game to the AI's Rate** (`MatchFrameRate`). Holds the game's frame rate (`t.MaxFPS`)
  near the AI's pictures a second while a cycle runs: at the rate where 85 % of the frames get a
  picture of their own (at least 20 frames a second). The frames the game no longer draws leave
  the AI more of the GPU; while the AI keeps up with more than 85 % of them, the hold rises, and
  while it misses more, it falls, so the two settle on their own. (A first version held the game
  5 % above the AI's rate; since the AI can never make more pictures than the game draws frames,
  that crept down with every hitch, to 22 frames a second.) `t.MaxFPS` is put back as it was when
  the cycle ends (and never raised above a cap that was already there).

**What it does, measured** (2026-10-20, 03:00, off-screen game at 1920 x 1080 with two players,
the AI kept in on player 1's screen at the start view, strength 0.6, depth 0.7, TensorRT, a
bridge of its own on channel 9; one run of about 70 s each, measured from 30 s into the cycle;
*new pictures* is what reaches the game, from its log, *long frames* those over 33 and 50 ms):

| | Game | New AI pictures a second | Frames with a new picture | Median frame | Long frames |
|---|---|---|---|---|---|
| Overlay off | 79 fps | | | 12.1 ms | 0.1 % / 0 % |
| As before 2026-10-20 (cap 15, not kept on the pieces, no blend) | 63 fps | 14.7 | 23 % | 12.8 ms | 5.5 % / 2.8 % |
| Cap 15, kept on the pieces, blended | 60 fps | 14.6 | 25 % | 12.9 ms | 6.5 % / 3.7 % |
| Cap 20, kept on the pieces, blended | 56 fps | 17.9 | 32 % | 13.0 ms | 9.3 % / 5.4 % |
| **The new defaults** (no cap, kept on the pieces, blended) | 54 fps | 19.2 | 35 % | 12.9 ms | 10.3 % / 6.7 % |
| New defaults + picture size 384 | 56 fps | 22.3 | 40 % | 13.0 ms | 9.5 % / 3.8 % |
| New defaults + held to the AI's rate | 26 fps | 19.6 (the bridge made about 22) | 72 % | 37.0 ms | 65 % / 11 % |
| New defaults + size 384 + held | 26 fps | 21.8 | 83 % | 37.7 ms | 68 % / 10 % |

So on this machine, the one RTX 4090 shared by a two-player game and the AI, the AI makes about
20 new pictures a second whatever is done: beside the game a picture of 14 ms takes 24 to 32 ms.
Every few more pictures a second cost the game a few frames and some stutter (the long frames
are the game waiting for the GPU while the AI has it). A picture for (nearly) every frame comes
only by holding the game down to the AI's rate, about 26 frames a second. What makes the
overlay look smooth at the game's full frame rate is keeping each picture on the pieces and
blending: with those the AI's look sits still on the pieces, at every frame, also at a cap of 15
(it then changes 15 times a second, softly). Not measured: play in the editor (more of the GPU
goes to the editor, so fewer pictures), one player, other places in the structure.

**Going back.** On the settings page: *Most Pictures a Second* 15, *Keep on the Pieces* off and
*Blend Between Pictures* off is the overlay as it was before 2026-10-20 (the bridge stays a
little faster per picture). The files as they were are kept in
`Saved/AIOverlay/before-smooth-2026-10-20` (copy them back over the new ones and rebuild).

## How it works

`Source/WEM_2026Rendering/Private/AIOverlay.cpp`, `Shaders/Private/AIOverlay.usf`,
`Tools/AIOverlay/bridge.py`.

- **The mask.** Every piece ARoomManager draws (beams, nodes and every structure's pieces) writes
  stencil value 1 into custom depth; with a share of segments below 100, only the picked segments
  and the beams and posts round them do, and the others write 0, so they still hide picked pieces behind them. The two kinds are kept
  in instanced chunks of their own (`AddPiecesToLayer`, `SortPiecesForOverlay` in
  `RoomManager.cpp`). The surface marks write nothing. This needs the project setting
  Custom Depth-Stencil Pass = Enabled with Stencil (`r.CustomDepth=3` in `DefaultEngine.ini`).
- **The cycle** runs on the wall clock (`AIOverlay::FCycle`): gap, ramp up, hold, ramp down.
  Between cycles the overlay adds no passes and sends nothing.
- **Sending.** During a cycle, a pass after tonemapping packs the frame into a 512 x 512 picture:
  the pieces in colour, everything else black, and the pieces' outline in alpha. When the bridge
  takes depth, a second pass draws the depth map below it, in the same texture, from scene depth
  where the custom stencil marks the pieces. The GPU copies it back (up to three frames in
  flight), and the newest finished one goes into Windows shared memory with the cycle's
  intensity, the strength, the depth weight and the prompt; the depth map goes into a third
  block of its own. For each picture sent, the game also keeps its camera (without the
  anti-aliasing jitter) and, in a small texture of its own, the nearest and furthest depth of the
  pieces under each of its pixels (PACK SENT DEPTH), for the last 24 pictures. Every frame, also
  between cycles, it writes its cap on the pictures a second and the picture size it wants into
  the input's header.
- **Fitting the depth.** The depth map's alpha carries the depth once more, on the fixed range
  from `DepthNear` to `DepthFar`. From each copy that comes back, the game counts how much of the
  pieces lies at each depth, takes the nearest and furthest (leaving out the outer 2 %, such as a
  beam right in front of the camera), and eases the map's range towards them
  (`FitDepthRange_RenderThread`). A range spans at least a ratio of 2, so a wall seen straight on
  does not turn every speck of its depth into black and white.
- **The bridge** waits for a new picture, hands the depth map to the ControlNet at the weight
  that came with it (none at weight 0), runs the picture through SD-Turbo at that strength, keeps
  the outline in alpha, and writes the result into a second block of shared memory. It tells the
  game in that block's header whether it takes depth, and the game sends none to a bridge that
  does not. Without a cap it looks for a new picture every millisecond; under one it sleeps until
  the next picture is due (it asks Windows for 1 ms sleeps rather than 15.6 ms) and then takes
  the newest one. When the game asks for another picture size, it loads the model again at it.
- **Laying it over.** The newest result is uploaded and blended over the frame by
  `Opacity x intensity`, only where the pieces are now (softened over a pixel's neighbours). With
  `Reproject`, each pixel of the pieces is turned back into a point in the world from the scene
  depth, taken through the camera kept for the picture to where that picture saw it, and counts
  as seen there if its depth then lies within the depths kept around that spot (3 % either way);
  the picture before fills in what the newest did not see, and with `Blend` the newest fades in
  over it. With `ClipToMask` the AI's picture covers only what was seen, and only where the
  pieces were when it was sent. The overlay runs before the sky glitch, so the glitch's trails
  carry the AI's look, and before the phone camera.
- **Holding the frame rate** (`MatchFrameRate`) is done on the game thread at the start of each
  frame, from the AI's pictures a second as the bridge counts them (it writes them into its
  header; the game cannot count them itself, since two pictures that reach it in the same frame
  look like one there, and a hold built on that count sank as the structure grew); when no game
  view has drawn for half a second (the play session ended), `t.MaxFPS` goes back.
- **The log** says every 5 seconds of a cycle how many frames the game drew and how many new
  pictures it took from the AI (`LogAIOverlay: AI overlay: 52.2 frames a second, 18.1 new
  pictures a second from the AI (35% of frames)`).

The shared memory layout is `AIOverlay::FSharedHeader` in `AIOverlay.h`, mirrored at the top of
`bridge.py`. The depth weight and the bridge's controls took two of the header's spare words, and
the game's cap and picture size two more, so the layout and its version stayed as they were and
an older bridge still works with the game (without depth, and at its own cap and size); the
`WEM.AIOverlay.SharedLayout` test checks the offsets and the channel names. `WEM.AIOverlay.Cycle`
checks the timing, and `WEM.AIOverlay.Settings` that the settings page sets its console
variables, `WEM.AIOverlay.Constant` the constant mode, `WEM.AIOverlay.SegmentShare` that the
pieces are sorted by the share and stay sorted as the structure grows, and
`WEM.AIOverlay.Reproject` that a spot seen now is taken back to where an earlier camera saw it,
in floats as on the GPU. The toolbar button is in `WEM_2026EditorModule.cpp`.

## Limits

- Until 2026-10-20 the AI made at most 15 pictures a second, the bridge's default cap then.
  Without a cap it made 25 to 28 a second with TensorRT and 11 to 13 without (512 x 512,
  measured 2026-10-20 in an off-screen game with one player, see below). In the editor's own play
  it was fewer: 5 to 11 with TensorRT in one session right after, and 6 to 10 without on
  2026-10-19. So the pieces changed in steps rather than smoothly; see *Smoothness* for what
  changed and what it makes now. The numbers below this point are from before that.
- What it costs the game, measured 2026-10-20 in an off-screen game at 1920 x 1080 (the start
  view, the first five minutes of growth, the editor open but not playing, cycles of 30 s at full
  and 30 s off), from Unreal Insights GPU traces:

  | | Between cycles | During a cycle, bridge without TensorRT | During a cycle, bridge with TensorRT |
  |---|---|---|---|
  | Game frame rate | 72 to 74 fps (13.5 to 13.9 ms a frame) | 52 fps (19.1 ms) | 46 fps (21.9 ms) |
  | GPU time a frame | 12.4 to 13.0 ms | 18.7 ms | 21.5 ms |
  | Custom depth pass (the mask) | 0.44 to 0.55 ms | 0.98 ms | 0.93 ms |
  | All post processing | 0.90 ms | 1.99 ms | 2.74 ms |

  The custom depth pass that draws every piece a second time for the mask runs on every frame,
  cycle or not: about half a millisecond, 4 % of the frame. The overlay's own passes are small:
  the pack, unpack and composite take about 0.01 ms each, and the GPU copy that sends the
  picture about 0.4 ms. Most of the drop is the bridge itself: SD-Turbo runs on the same GPU,
  and every pass of the game gets slower while it does (the custom depth pass nearly doubles).
  Between cycles the game adds no passes and the bridge sleeps, so the first column is also the
  game without the bridge.
- So TensorRT doubles the pictures a second but does not give the game its frames back; it costs
  a few more. The bridge works flat out whenever the game sends pictures (about 50 a second), so
  a faster bridge makes more pictures rather than taking less of the GPU.
- Hence the cap, `--max-rate`, 15 pictures a second by default. Measured later the same evening
  the same way (TensorRT, off-screen game at 1920 x 1080, cycles of 30 s at full and 30 s off),
  one run of five and a half minutes per cap:

  | Cap | Pictures a second | Game between cycles | Game during a cycle | Frames lost |
  |---|---|---|---|---|
  | 10 | 9.2 to 9.9 | 49.5 fps (see below) | 41.6 fps | 16 % |
  | **15 (the default)** | 14.8 to 15.1 | 69.8 fps | 58.9 fps | 16 % |
  | 20 | 18.6 to 19.9 | 70.2 fps | 56.8 fps | 19 % |
  | 0 (no cap) | 21.0 to 22.6 | 68.8 fps | 53.3 fps | 23 % |

  The frame rates are frames over time from the game thread. The GPU time of the median frame
  during a cycle tells the same story: 12.2 ms at 15, 15.8 ms at 20 and 19.0 ms without a cap,
  against 11.0 to 11.5 ms between cycles. In every run the game also hitches by itself, cycle
  or not (the median frame is 11 to 12 ms, but one in twenty takes 33 ms or more).

  The cap-10 run is not like the others: the editor was in the foreground and drew its own
  viewport throughout, which took a large share of the GPU (the game made only about 50 fps
  between cycles), and a play session started in the editor near the end froze everything for
  a while (left out). It lost the same share of frames as cap 15, so going below 15 seems to buy
  little, but a clean cap-10 run would settle it. Uncapped, the bridge made fewer pictures than
  in the first measurement (21 to 23 against 25 to 28), so the GPU was somewhat busier this time.

  So 15 keeps more pictures a second than the bridge without TensorRT ever made, for about 70 %
  of the uncapped cost. Even so, a cycle still costs about a sixth of the frames.
- **Depth** (measured 2026-10-19/20, off-screen game at 1920 x 1080, the AI held in at strength
  0.9, TensorRT, cap 15, the editor open, CSV profiler, frames 1500 to 4000 of each run): the
  bridge still makes 15 pictures a second with depth. The game made 105 fps with the overlay off,
  97 fps with it on and depth weight 0 (the ControlNet left out), and 88 fps with depth 0.7, in
  two runs alike (88.0 and 87.5); mean GPU time 9.0 ms against 10.3 and 9.9 ms. So depth costs
  about a tenth of the frames on top of the overlay. One more run without depth was disturbed
  (GPU time doubled) and is left out, and the bridge's rate without a cap was not measured. The
  TensorRT engines for depth took 21 minutes to build. Without TensorRT, the ControlNet adds
  about 27 ms a picture (73 to 100 ms).
- The AI's picture is a few frames behind the game. Kept on the pieces (*Smoothness*), that no
  longer shows while the camera moves, but a piece that has just appeared, or come out from
  behind another, has no AI picture until the next one comes (it keeps its own look meanwhile).
- SD-Turbo works one frame at a time, so the pieces "boil" a little from picture to picture; the
  blend softens it.
- The AI is stretched to 512 x 512 and back, whatever the window's shape.
- Models drift WBS70 panels toward Western-looking buildings; a prompt helps, a Plattenbau LoRA
  would fix it.
- **TensorRT** works since 2026-10-20 (`tensorrt` 10.12.0.36, plus `diffusers-ipadapter`, which
  StreamDiffusion's TensorRT export imports; its `setup.py` lists it under the `ipadapter` extra).
  Start the bridge with it:

  ```
  Tools\AIOverlay\run_bridge.bat --acceleration tensorrt
  ```

  The first start built the engines into `Saved/AIOverlay/engines` in 13 minutes; later starts
  load them in about a minute. Measured with the editor open (it keeps the GPU about a quarter
  busy):

  | | TensorRT | Without |
  |---|---|---|
  | The model alone, one picture | 31 ms | 75 ms |
  | The bridge fed test pictures, no game | 28 to 30 pictures a second | about 19 |
  | The bridge serving the off-screen game, no cap | 25 to 28 pictures a second | 11 to 13 |
  | GPU memory the bridge holds | 9.7 GB | 3.2 GB |

  The memory is the catch. In a first try on 2026-10-20 the TensorRT bridge, the editor, the
  off-screen game and the Windows Photos app (5.3 GB) together asked for more than the card's
  24 GB, and both the game and the bridge crawled (0.1 pictures a second, 120 ms frames); with
  Photos closed it fit. If a cycle makes the game stutter badly, close other programs that use
  the GPU, or go back to `--acceleration none`.

  `pypi.nvidia.com`, where TensorRT's libraries come from, times out from this network most of
  the time; the install got through once.
