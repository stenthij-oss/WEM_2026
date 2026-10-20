"""The AI half of WEM_2026's AI overlay (Source/WEM_2026Rendering/Private/AIOverlay.h).

While an overlay cycle runs, the game writes a small picture of the platform pieces on screen (everything else black,
the pieces' outline in alpha) into shared memory, along with how far into the cycle it is. This turns each new picture
into the AI's version with StreamDiffusion and SD-Turbo, and writes it back for the game to lay over the pieces.

With the depth ControlNet (downloaded with the model, and on unless --no-depth), the game also sends the pieces' depth
with each picture, and the AI keeps their shape and what is in front of what, even at a strength that repaints their
colours completely. How strongly is the game's to say (wem.AIOverlay.DepthWeight); 0 skips the ControlNet.

The model is loaded once, at the start, which takes a minute (and the first time, building the TensorRT engines takes
about ten; with the depth ControlNet they are engines of their own, built once more). Between cycles no pictures come,
and this only sleeps. Fetch the model first with download_model.py.

    Tools\\AIOverlay\\run_bridge.bat                       (the defaults)
    Tools\\AIOverlay\\run_bridge.bat --prompt "..."        (what to turn the pieces into)
    Tools\\AIOverlay\\run_bridge.bat --max-rate 15         (at most 15 pictures a second, as it was until 2026-10-20)
    Tools\\AIOverlay\\run_bridge.bat --no-depth            (without the depth ControlNet: faster, less GPU memory)

The prompt can also be changed while the game runs, from its console: wem.AIOverlay.Prompt "...".

The game sends a new picture every frame, and by default this makes one from every picture it can, always the newest:
the AI follows the game's frame rate as closely as the GPU they share allows. --max-rate, or the game's own cap
(wem.AIOverlay.MaxRate, on the AI Overlay settings page), holds it to fewer pictures a second, to leave the game more
of the GPU; between them it sleeps. The game can also ask for another picture size (wem.AIOverlay.Size); the bridge
then loads the model again at that size, which takes about half a minute. Structures/AI_OVERLAY.md has what each costs.

--channel N pairs this bridge with games started with -AIOverlayChannel=N only (0, the default, is the editor's and the
game's usual one), so a second game, such as a measurement run, does not take the pictures of the first.

Only one bridge runs at a time on a channel: a second one stops at once. A packaged game starts the bridge itself,
without a window, and stops it when it quits (Source/WEM_2026Rendering/Private/AIBridgeLauncher.h, Structures/BUILD.md).

Each picture goes to the GPU in one go and is waited for once (Painter.run_without_waits); --sync-steps waits after every
step instead, as StreamDiffusion does on its own (a little slower).
"""

import argparse
import gc
import mmap
import os
import struct
import sys
import time
from pathlib import Path

import numpy as np

PROJECT = Path(__file__).resolve().parents[2]
CACHE = PROJECT / "Saved" / "AIOverlay"
# Where download_model.py puts the model; without it, the bridge fetches it from Hugging Face itself.
LOCAL_MODEL = CACHE / "models" / "sd-turbo"
LOCAL_TINY_DECODER = CACHE / "models" / "taesd"
LOCAL_DEPTH = CACHE / "models" / "controlnet-depth"

# The model downloads go to the project's Saved folder rather than C:, which is nearly full.
os.environ.setdefault("HF_HOME", str(CACHE / "hf"))

# Must match AIOverlay::FSharedHeader and the constants after it in AIOverlay.h.
MAGIC = 0x414D4557  # "WEMA"
VERSION = 1
HEADER = struct.Struct("<6I2f2IfIfIfI")  # Magic, Version, Sequence, Width, Height, FrameId, Intensity, Strength,
                                         # Heartbeat, PromptLength, DepthWeight, Controls, MaxRate, Size, Rate, Reserved
SEQUENCE_OFFSET = 8
HEARTBEAT_OFFSET = 32
RATE_OFFSET = 56
CONTROL_DEPTH = 1
PROMPT_OFFSET = HEADER.size
PROMPT_SIZE = 1024
PIXEL_OFFSET = 2048
MAX_SIDE = 1024
SHARED_SIZE = PIXEL_OFFSET + MAX_SIDE * MAX_SIDE * 4
INPUT_NAME = "{5E1A7C3B-2D44-4F0E-9B6A-41A0C3D2E701}"
OUTPUT_NAME = "{5E1A7C3B-2D44-4F0E-9B6A-41A0C3D2E702}"
# The game's depth map, at PIXEL_OFFSET like its picture, written with it (under the input's Sequence). No header.
CONTROL_NAME = "{5E1A7C3B-2D44-4F0E-9B6A-41A0C3D2E703}"
# Held while a bridge runs, so a second one on its channel does not start. Must match BridgeMutexName in
# AIBridgeLauncher.cpp; on channel N it has ".N" after it.
BRIDGE_MUTEX = "WEM_2026.AIOverlay.Bridge"
ERROR_ALREADY_EXISTS = 183

# The smallest picture side the TensorRT engines take: the depth ControlNet's are built for 384 to 1024.
MIN_SIDE = 384

DEFAULT_PROMPT = "weathered prefabricated concrete panel building, East German Plattenbau, photograph, overcast light"
DEFAULT_NEGATIVE = "blurry, low quality, text, watermark"

# StreamDiffusion splits the noise schedule into this many steps. Index 0 starts from pure noise, the last index
# from the picture itself, so a strength of s starts at index (1 - s) * (STEPS - 1).
STEPS = 50


def strength_to_t_index(strength: float) -> int:
    strength = min(max(strength, 0.0), 1.0)
    return round((1.0 - strength) * (STEPS - 1))


def shared_name(name: str, channel: int) -> str:
    """The name of a block of shared memory on a channel, as AIOverlay::GetSharedName in AIOverlay.cpp: the channel is
    added to the GUID's second group of digits (the game can share a block only under a bare {GUID} name)."""
    if channel == 0:
        return name
    group = (int(name[10:14], 16) + channel) & 0xFFFF
    return f"{name[:10]}{group:04X}{name[14:]}"


def read_u32(buffer: mmap.mmap, offset: int) -> int:
    return struct.unpack_from("<I", buffer, offset)[0]


def write_u32(buffer: mmap.mmap, offset: int, value: int) -> None:
    struct.pack_into("<I", buffer, offset, value & 0xFFFFFFFF)


def read_input(shared_in: mmap.mmap, shared_control: mmap.mmap, last_frame: int, pixels: np.ndarray, depth: np.ndarray):
    """Copies the game's newest picture into pixels (and its depth map into depth, when one came with it), and returns
    (header fields, prompt, whether a depth map came), or None if there is no new whole one."""
    before = read_u32(shared_in, SEQUENCE_OFFSET)
    if before & 1:
        return None

    (magic, version, _, width, height, frame_id, intensity, strength, _, prompt_length, depth_weight,
     *_) = HEADER.unpack_from(shared_in, 0)
    if magic != MAGIC or version != VERSION or frame_id == last_frame:
        return None
    if not (0 < width <= MAX_SIDE and 0 < height <= MAX_SIDE):
        return None

    size = width * height * 4
    prompt = bytes(shared_in[PROMPT_OFFSET:PROMPT_OFFSET + min(prompt_length, PROMPT_SIZE)])
    # Straight from shared memory into the pinned buffers the GPU copies from.
    np.copyto(pixels[:size], np.frombuffer(shared_in, dtype=np.uint8, count=size, offset=PIXEL_OFFSET))
    # The game sends a depth map only when it gives it a weight.
    has_depth = depth_weight > 0.0
    if has_depth:
        np.copyto(depth[:size], np.frombuffer(shared_control, dtype=np.uint8, count=size, offset=PIXEL_OFFSET))

    # Written over while it was copied: wait for the next one.
    if read_u32(shared_in, SEQUENCE_OFFSET) != before:
        return None

    fields = (width, height, frame_id, intensity, strength, depth_weight)
    return fields, prompt.decode("utf-8", errors="replace"), has_depth


def read_wishes(shared_in: mmap.mmap):
    """The game's cap on the pictures a second (0 = none of its own) and the picture size it asks for (0 = the
    bridge's own), from the input's header; (0, 0) before the game has written one."""
    magic, version = struct.unpack_from("<2I", shared_in, 0)
    if magic != MAGIC or version != VERSION:
        return 0.0, 0
    max_rate, size = struct.unpack_from("<fI", shared_in, 48)
    return max(max_rate, 0.0), size


def write_output(shared_out: mmap.mmap, width: int, height: int, frame_id: int, pixels: np.ndarray) -> None:
    sequence = read_u32(shared_out, SEQUENCE_OFFSET) | 1
    write_u32(shared_out, SEQUENCE_OFFSET, sequence)
    struct.pack_into("<3I", shared_out, 12, width, height, frame_id)
    shared_out[PIXEL_OFFSET:PIXEL_OFFSET + pixels.size] = memoryview(pixels)
    write_u32(shared_out, SEQUENCE_OFFSET, sequence + 1)


def write_ready(shared_out: mmap.mmap, width: int, height: int, controls: int) -> None:
    """The output's header as the game first sees it: the size the AI works at, and the maps it takes. Only once this is
    written, and the heartbeat moves, does the game send pictures."""
    sequence = read_u32(shared_out, SEQUENCE_OFFSET) & ~1
    HEADER.pack_into(shared_out, 0, MAGIC, VERSION, sequence, width, height, 0, 0.0, 0.0, read_u32(shared_out, HEARTBEAT_OFFSET),
                     0, 0.0, controls, 0.0, 0, 0, 0)


class _NoStream:
    """Stands in for torch.cuda.stream(...) when the bridge's GPU work stays on torch's own stream."""

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        return False


class _NoWaitStream:
    """A TensorRT engine's CUDA stream whose synchronize() does not wait (see Painter.run_without_waits)."""

    def __init__(self, stream):
        self.ptr = stream.ptr

    def synchronize(self):
        pass


def _infer_without_wait(self, feed_dict, stream, use_cuda_graph=False):
    """StreamDiffusion's Engine.infer (acceleration/tensorrt/utilities.py) without the wait after a CUDA graph."""
    from streamdiffusion.acceleration.tensorrt import utilities

    inputs = {self.engine.get_tensor_name(i) for i in range(self.engine.num_io_tensors)
              if self.engine.get_tensor_mode(self.engine.get_tensor_name(i)) == utilities.trt.TensorIOMode.INPUT}
    for name, buffer in feed_dict.items():
        if name in inputs:
            self.tensors[name].copy_(buffer)
    for name, tensor in self.tensors.items():
        self.context.set_tensor_address(name, tensor.data_ptr())

    if use_cuda_graph and self.cuda_graph_instance is not None:
        utilities.CUASSERT(utilities.cudart.cudaGraphLaunch(self.cuda_graph_instance, stream.ptr))
    elif use_cuda_graph:
        # The first time: run it, then record it for the next times, as StreamDiffusion does.
        if not self.context.execute_async_v3(stream.ptr):
            raise ValueError("ERROR: inference failed.")
        utilities.CUASSERT(utilities.cudart.cudaStreamBeginCapture(
            stream.ptr, utilities.cudart.cudaStreamCaptureMode.cudaStreamCaptureModeGlobal))
        self.context.execute_async_v3(stream.ptr)
        self.graph = utilities.CUASSERT(utilities.cudart.cudaStreamEndCapture(stream.ptr))
        self.cuda_graph_instance = utilities.CUASSERT(utilities.cudart.cudaGraphInstantiate(self.graph, 0))
    elif not self.context.execute_async_v3(stream.ptr):
        raise ValueError("ERROR: inference failed.")
    return self.tensors


class Painter:
    """The AI, built for one picture size: StreamDiffusion with SD-Turbo, and the depth ControlNet when there is one."""

    def __init__(self, args, depth_model, width: int, height: int):
        import torch
        from streamdiffusion import StreamDiffusionWrapper

        self.torch = torch
        self.width = width
        self.height = height
        self.depth_model = depth_model
        self.t_index = strength_to_t_index(0.5)
        self.depth_weight = 0.0

        with_depth = f", depth ControlNet {depth_model}" if depth_model else ""
        print(f"Loading {args.model} at {width}x{height} ({args.acceleration}{with_depth})...", flush=True)
        self.wrapper = StreamDiffusionWrapper(
            model_id_or_path=args.model,
            t_index_list=[self.t_index],
            mode="img2img",
            output_type="pt",
            frame_buffer_size=1,
            width=width,
            height=height,
            warmup=10,
            acceleration=args.acceleration,
            use_denoising_batch=True,
            use_tiny_vae=True,
            cfg_type="none",
            seed=args.seed,
            vae_id=str(LOCAL_TINY_DECODER) if (LOCAL_TINY_DECODER / "config.json").exists() else None,
            # A UNet built to take a ControlNet's help is a different engine under the same name: keep the two apart.
            # The engines take any size from 384 to 1024, so another size needs no engines of its own.
            engine_dir=str(CACHE / ("engines-depth" if depth_model else "engines")),
            use_controlnet=depth_model is not None,
            # No preprocessor: the game's depth map goes in as it is. The weight comes with each picture.
            controlnet_config=[{"model_id": depth_model, "conditioning_scale": 0.0}] if depth_model else None,
        )
        self.stream = self.wrapper.stream
        engines = getattr(self.stream, "controlnet_engines", None) or []
        if depth_model and not engines:
            # Without a TensorRT engine for it, the ControlNet runs as it was loaded, and StreamDiffusion leaves it in
            # main memory.
            for controlnet in self.stream._controlnet_module.controlnets:
                controlnet.to("cuda")
        for engine in engines:
            # StreamDiffusion replays the UNet's engine as one CUDA graph, but launches the ControlNet's step by step;
            # replayed too, a picture takes 1.5 ms less (5.3 ms of 18 went to the ControlNet, measured 2026-10-20).
            engine.use_cuda_graph = True
        self.wrapper.prepare(prompt=args.prompt, negative_prompt=args.negative, num_inference_steps=STEPS,
                             guidance_scale=1.0)
        self.prompt = args.prompt
        self.cuda_stream = None if args.sync_steps else self.run_without_waits(engines)

        # Page-locked buffers, so the copies to and from the GPU run at full speed and need no buffer of their own.
        side = MAX_SIDE * MAX_SIDE * 4
        self.pinned_in = torch.empty(side, dtype=torch.uint8).pin_memory()
        self.pinned_depth = torch.empty(side, dtype=torch.uint8).pin_memory()
        self.pinned_out = torch.empty(side, dtype=torch.uint8).pin_memory()
        self.pixels = self.pinned_in.numpy()
        self.depth = self.pinned_depth.numpy()
        self.out = self.pinned_out.numpy()

    def run_without_waits(self, controlnet_engines):
        """
        Every TensorRT engine of StreamDiffusion's runs on one CUDA stream, and after each engine it waits for the GPU to
        finish. This runs the bridge's own steps on that same stream too, so they keep their order without waiting, and
        leaves out the waits after the engines: a picture goes to the GPU in one go and is waited for once, at the end
        (measured 2026-10-20: 65 to 70 pictures a second alone, 18 to 19 beside the game). Returns that stream as
        torch's, or None when the engines are not all TensorRT ones on one stream (then everything runs as
        StreamDiffusion has it).
        """
        torch = self.torch
        stream = self.stream
        engine_stream = getattr(stream.unet, "stream", None)
        if engine_stream is None or not hasattr(stream.vae, "encoder"):
            return None
        if any(getattr(engine, "stream", engine_stream).ptr != engine_stream.ptr for engine in [stream.vae, *controlnet_engines]):
            print("The TensorRT engines run on different CUDA streams; the bridge waits after each of them.", flush=True)
            return None

        from streamdiffusion.acceleration.tensorrt.utilities import Engine
        Engine.infer = _infer_without_wait
        for engine in controlnet_engines:
            engine.stream = _NoWaitStream(engine.stream)
        return torch.cuda.ExternalStream(engine_stream.ptr)

    def on_stream(self):
        """Where the bridge's GPU work runs: the engines' stream (see run_without_waits), or torch's own."""
        return self.torch.cuda.stream(self.cuda_stream) if self.cuda_stream is not None else _NoStream()

    def set_prompt(self, prompt: str) -> None:
        if prompt != self.prompt:
            self.prompt = prompt
            with self.on_stream():
                self.wrapper.update_prompt(prompt)
            self.torch.cuda.synchronize()
            print(f"Prompt: {prompt}", flush=True)

    def paint(self, width: int, height: int, strength: float, depth_weight: float, has_depth: bool) -> np.ndarray:
        """The AI's version of the picture in self.pixels (and self.depth), as RGBA8 bytes of the same size."""
        with self.on_stream():
            return self._paint(width, height, strength, depth_weight, has_depth)

    def _paint(self, width: int, height: int, strength: float, depth_weight: float, has_depth: bool) -> np.ndarray:
        torch = self.torch
        stream = self.stream
        size = width * height * 4

        wanted = strength_to_t_index(strength)
        if wanted != self.t_index:
            self.t_index = wanted
            self.wrapper.update_stream_params(t_index_list=[self.t_index])
            torch.cuda.synchronize()

        image = self.pinned_in[:size].to("cuda", non_blocking=True).view(height, width, 4)
        # -1..1, as the model takes it.
        rgb = image[..., :3].permute(2, 0, 1).unsqueeze(0).to(torch.float16) * (2.0 / 255.0) - 1.0
        resized = (width, height) != (self.width, self.height)
        if resized:
            rgb = torch.nn.functional.interpolate(rgb, size=(self.height, self.width), mode="bilinear", align_corners=False)

        if self.depth_model:
            # The weight first: a map handed over at weight 0 is dropped.
            wanted_weight = depth_weight if has_depth else 0.0
            if wanted_weight != self.depth_weight:
                self.depth_weight = wanted_weight
                stream._controlnet_module.update_controlnet_scale(0, self.depth_weight)
            if has_depth:
                # Depth in red, made grey: the ControlNet knows depth maps as grey pictures.
                near = self.pinned_depth[:size].to("cuda", non_blocking=True).view(height, width, 4)[..., 0]
                grey = (near.to(torch.float16) / 255.0).expand(1, 3, height, width)
                if resized:
                    grey = torch.nn.functional.interpolate(grey, size=(self.height, self.width), mode="bilinear",
                                                           align_corners=False)
                self.wrapper.update_control_image(0, grey.contiguous())

        # The pipeline's own steps, without the wrapper's checks, conversions and waits around them: the same picture,
        # 1.2 ms sooner. Its ControlNet runs inside the UNet step.
        result = stream.decode_image(stream.predict_x0_batch(stream.encode_image(rgb)))
        if resized:
            result = torch.nn.functional.interpolate(result, size=(height, width), mode="bilinear", align_corners=False)

        # The AI's colours, with the outline of the pieces it was given, so the game can cut to it.
        colour = ((result[0] + 1.0) * 127.5).clamp(0, 255).round().to(torch.uint8).permute(1, 2, 0)
        self.pinned_out[:size].view(height, width, 4).copy_(torch.cat([colour, image[..., 3:4]], dim=-1), non_blocking=True)
        # The one wait for the whole picture.
        torch.cuda.current_stream().synchronize()
        return self.out[:size]


def fit_side(value: int) -> int:
    """A picture side the engines take: a multiple of 64 from MIN_SIDE to MAX_SIDE."""
    return min(max(value // 64 * 64, MIN_SIDE), MAX_SIDE)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--prompt", default=DEFAULT_PROMPT, help="what to turn the pieces into")
    parser.add_argument("--negative", default=DEFAULT_NEGATIVE, help="what to keep out of them")
    parser.add_argument("--model", default=str(LOCAL_MODEL) if (LOCAL_MODEL / "model_index.json").exists() else "stabilityai/sd-turbo",
                        help="a Hugging Face model id or a local folder; its name must contain 'turbo' for SD-Turbo")
    parser.add_argument("--width", type=int, default=512,
                        help="the size the AI works at unless the game asks for another; a multiple of 64")
    parser.add_argument("--height", type=int, default=512)
    parser.add_argument("--acceleration", default="tensorrt", choices=["tensorrt", "xformers", "none"])
    parser.add_argument("--seed", type=int, default=2)
    parser.add_argument("--max-rate", type=float, default=0.0,
                        help="at most this many pictures a second, unless the game sets its own cap; 0 (the default) = "
                             "one from every picture the game sends, as fast as the GPU allows")
    parser.add_argument("--depth", default=str(LOCAL_DEPTH),
                        help="the depth ControlNet: a local folder (an SD 2.1 one, since SD-Turbo is SD 2.1 underneath)")
    parser.add_argument("--no-depth", action="store_true", help="run without the depth ControlNet")
    parser.add_argument("--channel", type=int, default=0,
                        help="serve only games started with -AIOverlayChannel=N; 0 = the usual one")
    parser.add_argument("--sync-steps", action="store_true",
                        help="wait for the GPU after every step of a picture, as StreamDiffusion does on its own (a "
                             "little slower); for comparing, or if pictures ever come back garbled")
    args = parser.parse_args()

    if args.width % 64 or args.height % 64 or not (64 <= args.width <= MAX_SIDE and 64 <= args.height <= MAX_SIDE):
        parser.error(f"--width and --height must be multiples of 64 from 64 to {MAX_SIDE}")
    if args.max_rate < 0:
        parser.error("--max-rate must be 0 (no cap) or more")
    if args.channel < 0:
        parser.error("--channel must be 0 or more")

    # One bridge at a time on a channel: two would answer the same game, each with half the GPU. A packaged game that
    # starts the bridge itself looks for this too, and uses a running bridge instead of starting another.
    if sys.platform == "win32":
        import ctypes
        kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel32.CreateMutexW.restype = ctypes.c_void_p
        mutex_name = BRIDGE_MUTEX if args.channel == 0 else f"{BRIDGE_MUTEX}.{args.channel}"
        if kernel32.CreateMutexW(None, False, mutex_name) and ctypes.get_last_error() == ERROR_ALREADY_EXISTS:
            on_channel = f" on channel {args.channel}" if args.channel else ""
            print(f"Another bridge is already running{on_channel}; this one stops.", flush=True)
            return 1

    depth_model = None
    if not args.no_depth:
        depth_path = Path(args.depth).resolve()
        if not (depth_path / "config.json").exists():
            print(f"No depth ControlNet at {depth_path} (fetch it with download_model.py); running without.", flush=True)
        else:
            # StreamDiffusion names a ControlNet's TensorRT engine folder after the path it is given, so the path must not
            # carry a drive letter: give it relative to the project, from the project.
            os.chdir(PROJECT)
            try:
                depth_model = Path(os.path.relpath(depth_path, PROJECT)).as_posix()
            except ValueError:  # on another drive
                parser.error("--depth must be on the same drive as the project")

    # Windows wakes a sleeping thread only every 15.6 ms unless asked for finer steps, which would make the cap's
    # waits (and the 1 ms poll below) up to a tick too long. Windows undoes this when the process ends.
    if sys.platform == "win32":
        import ctypes
        ctypes.WinDLL("winmm").timeBeginPeriod(1)

    # Created by whichever side starts first, opened by the other.
    shared_in = mmap.mmap(-1, SHARED_SIZE, tagname=shared_name(INPUT_NAME, args.channel))
    shared_out = mmap.mmap(-1, SHARED_SIZE, tagname=shared_name(OUTPUT_NAME, args.channel))
    shared_control = mmap.mmap(-1, SHARED_SIZE, tagname=shared_name(CONTROL_NAME, args.channel))

    own_size = (args.width, args.height)
    asked_size = 0
    wished_size, wished_at = 0, 0.0
    painter = Painter(args, depth_model, *own_size)
    controls = CONTROL_DEPTH if depth_model else 0

    # Only now does the game see a heartbeat, so it does not send pictures to a bridge still loading.
    write_ready(shared_out, painter.width, painter.height, controls)
    on_channel = f" on channel {args.channel}" if args.channel else ""
    cap = f"at most {args.max_rate:g} pictures a second" if args.max_rate else "a picture from every one the game sends"
    print(f"Ready{on_channel} ({cap}). Waiting for the game's overlay cycles (Ctrl+C to stop).", flush=True)

    heartbeat = read_u32(shared_out, HEARTBEAT_OFFSET) + 1
    last_frame = 0
    made = 0
    report_time = time.perf_counter()
    report_made = 0
    painting = 0.0
    last_finished, rate_interval = 0.0, 0.0
    last_picture_time = 0.0
    next_picture_time = 0.0
    working = False
    last_cap = None

    try:
        while True:
            heartbeat += 1
            write_u32(shared_out, HEARTBEAT_OFFSET, heartbeat)

            game_rate, game_size = read_wishes(shared_in)

            # The game asks for another picture size (or, with 0, for the bridge's own): load the model again at it, once
            # the wish has held for a second, so a game that sets its size just after it starts costs one load, not two.
            if game_size != wished_size:
                wished_size, wished_at = game_size, time.perf_counter()
            if wished_size != asked_size and time.perf_counter() - wished_at >= 1.0:
                asked_size = wished_size
                wanted = (fit_side(asked_size), fit_side(asked_size)) if asked_size else own_size
                if wanted != (painter.width, painter.height):
                    print(f"The game asks for {wanted[0]}x{wanted[1]} pictures; loading the model again "
                          "(the AI pauses meanwhile).", flush=True)
                    prompt = painter.prompt
                    del painter
                    gc.collect()
                    import torch
                    torch.cuda.empty_cache()
                    painter = Painter(args, depth_model, *wanted)
                    painter.set_prompt(prompt)
                    write_ready(shared_out, painter.width, painter.height, controls)
                    print("Ready again.", flush=True)
                    continue

            # Under a cap, wait for the next picture's turn, in short sleeps so the heartbeat keeps moving.
            rate = game_rate if game_rate > 0.0 else args.max_rate
            interval = 1.0 / rate if rate > 0.0 else 0.0
            if interval != last_cap:
                if last_cap is not None:
                    print(f"Cap: {f'{rate:g} pictures a second' if interval else 'none'}.", flush=True)
                last_cap = interval
            if interval:
                wait = next_picture_time - time.perf_counter()
                if wait > 0.0:
                    time.sleep(min(wait, 0.1))
                    continue

            picture = read_input(shared_in, shared_control, last_frame, painter.pixels, painter.depth)
            if picture is None:
                if working and time.perf_counter() - last_picture_time > 1.0 + interval:
                    print("Cycle over; sleeping.", flush=True)
                    working = False
                    struct.pack_into("<f", shared_out, RATE_OFFSET, 0.0)
                # The game sends a picture a frame: look again in a millisecond, so one is taken soon after it comes.
                time.sleep(0.001)
                continue

            (width, height, frame_id, intensity, strength, game_depth_weight), game_prompt, has_depth = picture
            last_frame = frame_id
            last_picture_time = time.perf_counter()
            # Keep to the schedule, so waking a little late does not lower the rate; after a gap, start a new one.
            if last_picture_time - next_picture_time < interval:
                next_picture_time += interval
            else:
                next_picture_time = last_picture_time + interval
            if not working:
                print("Cycle started.", flush=True)
                working = True
                # The rate counts from here, not from the last cycle's final report.
                report_time = last_picture_time
                report_made = made
                painting = 0.0
                last_finished, rate_interval = 0.0, 0.0

            painter.set_prompt(game_prompt or args.prompt)
            paint_start = time.perf_counter()
            rgba = painter.paint(width, height, strength, game_depth_weight, has_depth)
            write_output(shared_out, width, height, frame_id, rgba)
            finished = time.perf_counter()
            painting += finished - paint_start

            # The pictures a second, over about the last second, for the game (wem.AIOverlay.MatchFrameRate): it cannot
            # count them itself, since two that reach it in the same frame look like one there.
            if last_finished:
                gap = finished - last_finished
                rate_interval = gap if not rate_interval else rate_interval + 0.1 * (gap - rate_interval)
                struct.pack_into("<f", shared_out, RATE_OFFSET, 1.0 / rate_interval)
            last_finished = finished

            made += 1
            now = time.perf_counter()
            if now - report_time >= 5.0:
                # How long a picture takes, beside whatever else the GPU does; the rest of the time it waits for pictures.
                pictures = made - report_made
                print(f"{pictures / (now - report_time):5.1f} pictures/s ({1000.0 * painting / max(pictures, 1):4.1f} ms "
                      f"each, busy {100.0 * painting / (now - report_time):3.0f}% of the time), intensity {intensity:.2f}, "
                      f"strength {strength:.2f} (t_index {painter.t_index}), depth weight {painter.depth_weight:.2f}",
                      flush=True)
                report_time = now
                report_made = made
                painting = 0.0
    except KeyboardInterrupt:
        print("Stopped.", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
