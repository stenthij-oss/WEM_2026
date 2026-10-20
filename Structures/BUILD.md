# The game build

WEM_2026 packaged as a game, so it runs without the editor. Made on 2026-10-20, on branch
`nature`, from the uncommitted work of that night. The build is in `Packaged\Windows` in the
project folder. Git ignores that folder.

## Playing it

1. Double-click `Packaged\Windows\WEM_2026.exe`.
2. The game starts the AI bridge by itself. The bridge has no window and runs in the background.
   It needs about 20 seconds to load the AI (up to a minute right after the computer starts).
   Until then the AI overlay cycles do nothing, so give it that time on the start screen before
   pressing A.
3. Press A to start. The game quits by itself when the music ends. Alt+F4 quits it any time.
4. When the game quits, the bridge it started stops too, and the GPU memory it held (about 10 GB)
   is free again.

The bridge is not inside the build. The game uses the one in this project: `Tools\AIOverlay`,
the Python environment in `StreamDiffusion\.venv`, and the model and TensorRT engines in
`Saved\AIOverlay`. The game finds the project by looking in the folders above its own. So keep
the build inside the project folder, or see *Settings* below.

## Is the AI running?

- `Packaged\Windows\WEM_2026\Saved\Logs\AIBridge.log` is everything the bridge prints. It says
  `Ready (...)` once the AI is loaded, `Cycle started.` when an overlay cycle comes, and the
  pictures per second during a cycle.
- `Packaged\Windows\WEM_2026\Saved\Logs\WEM_2026.log` is the game's own log. Its `LogAIBridge`
  lines say whether the game started the bridge, used one that was already running, or could not
  find it, and why.
- In Task Manager, the bridge shows as `python.exe` (two of them, with a `cmd.exe`).

## Running the bridge by hand instead

Use this when the bridge does not start by itself, or to see its window, or to try other options
without packaging again.

1. Open a terminal in the project folder and start the bridge. Options go after it, for example
   `--max-rate 10` or `--prompt "..."`:

   ```
   Tools\AIOverlay\run_bridge.bat
   ```

2. Wait until it prints `Ready`.
3. Start `Packaged\Windows\WEM_2026.exe`. The game sees the running bridge and uses it. It does
   not start a second one, and it leaves this one running when it quits.
4. Stop the bridge with Ctrl+C in its window.

Only one bridge runs at a time. A second one prints `Another bridge is already running; this one
stops.` and quits. To keep the game from starting a bridge at all, start it with `-NoAIBridge`
(for example from a shortcut: `WEM_2026.exe -NoAIBridge`).

## Settings

The bridge settings are on the AI Overlay page in the editor (Project Settings > Game > AI
Overlay > Bridge):

- **Start With a Packaged Game**: on by default. Off, the bridge is always started by hand.
- **Options**: what the game starts the bridge with, as for `run_bridge.bat`. Empty uses the
  bridge's defaults: TensorRT, the depth ControlNet, and as many pictures a second as the GPU
  allows, unless the page's **Most Pictures a Second** caps it.
- **Project Folder**: where the bridge is, for a build kept outside the project folder.

These and all the other AI overlay settings are copied into the build when it is packaged. To
change one in the finished build without packaging again, add it to
`Packaged\Windows\WEM_2026\Saved\Config\Windows\Game.ini` (make the file if it is not there):

```
[/Script/WEM_2026Rendering.AIOverlaySettings]
BridgeOptions=--max-rate 10
Prompt=what to turn the pieces into
```

The editor itself never starts the bridge. In the editor, start it by hand as before
(`Structures/AI_OVERLAY.md`).

## Making a new build

1. Save everything in the editor and close it. The packaging builds the editor too, and the
   editor's Live Coding blocks that while it runs.
2. Run this from the project folder:

   ```
   Tools\Package\package.bat
   ```

3. It builds, cooks and packages into `Packaged\Windows`, over the old build. That took 7
   minutes on 2026-10-20; new materials or shaders make it longer. The last line says `Done`.
   Its log is in the terminal. If it fails, the first `Error:` line in it says why.

It is a Development build: the console (`~`) works in it, and it writes the logs above. The
engine is expected at `D:\Unreal engine\UE_5.8`. Set `UE_ROOT` to use another.
