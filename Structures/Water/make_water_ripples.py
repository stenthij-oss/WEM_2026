"""
Makes the water's ripple normal map: a seamless tile, so the water material can repeat it across
the world with no seam.

The height field is white noise shaped in frequency space, so it is periodic by construction: every
wave in it fits the tile a whole number of times. The normals come from periodic differences, so the
tile's edges carry on into its other side exactly.

    python make_water_ripples.py [out.png] [--size 1024] [--seed 7] [--peak 6] [--spread 0.25] [--falloff 6] [--tilt 20]

Writes an 8-bit RGB normal map (tangent space, DirectX green, as Unreal reads it). Import it with
compression Normalmap and sRGB off.
"""

import argparse

import numpy as np
from PIL import Image


def make_height(size: int, seed: int, peak: float, spread: float, falloff: float) -> np.ndarray:
    rng = np.random.default_rng(seed)

    # Wave numbers in cycles per tile, so each wave repeats a whole number of times across it.
    k = np.fft.fftfreq(size, d=1.0 / size)
    kx, ky = np.meshgrid(k, k)
    radius = np.maximum(np.hypot(kx, ky), 1e-6)

    # A wind-sea spectrum in shape: little below the peak, then falling as k^-falloff. The lean of the
    # surface is what shows, and it takes k^2 more than the height does, so past 4 the ripples near the
    # peak carry the look and the fine ones only roughen it.
    spectrum = radius ** -falloff * np.exp(-1.25 * (peak / radius) ** 2)

    # Spread about one direction, both ways along it, so crests run in long ragged lines across it
    # rather than as round grain. Spread 0 is all one way, 1 is every way alike.
    along = (kx / radius) ** 2
    spectrum *= spread + (1.0 - spread) * along ** 2

    spectrum[radius < 1.5] = 0.0
    spectrum[radius > size * 0.35] = 0.0

    noise = rng.normal(size=(size, size)) + 1j * rng.normal(size=(size, size))
    height = np.real(np.fft.ifft2(noise * np.sqrt(spectrum)))
    return height / np.abs(height).max()


def make_normals(height: np.ndarray, mean_tilt: float) -> np.ndarray:
    # Central differences that wrap round the tile, so the edges match their opposite edges.
    du = (np.roll(height, -1, axis=1) - np.roll(height, 1, axis=1)) * 0.5
    dv = (np.roll(height, -1, axis=0) - np.roll(height, 1, axis=0)) * 0.5

    # Scaled so the surface leans mean_tilt degrees on average. The material can flatten it further.
    scale = np.tan(np.radians(mean_tilt)) / np.hypot(du, dv).mean()

    # Image rows run down, as Unreal's V does, so green is the DirectX way round.
    nx = -du * scale
    ny = dv * scale
    nz = np.ones_like(height)

    length = np.sqrt(nx * nx + ny * ny + nz * nz)
    return np.stack([nx / length, ny / length, nz / length], axis=-1)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("out", nargs="?", default="T_Water_Ripples_N.png")
    parser.add_argument("--size", type=int, default=1024)
    parser.add_argument("--seed", type=int, default=7)
    parser.add_argument("--peak", type=float, default=6.0, help="ripples per tile the most energy sits at")
    parser.add_argument("--spread", type=float, default=0.25, help="0 all one way, 1 every way alike")
    parser.add_argument("--falloff", type=float, default=6.0, help="how fast finer ripples fade")
    parser.add_argument("--tilt", type=float, default=20.0, help="mean lean of the surface, in degrees")
    args = parser.parse_args()

    normals = make_normals(make_height(args.size, args.seed, args.peak, args.spread, args.falloff), args.tilt)
    pixels = np.clip((normals * 0.5 + 0.5) * 255.0 + 0.5, 0, 255).astype(np.uint8)
    Image.fromarray(pixels, mode="RGB").save(args.out)

    tilt = np.degrees(np.arccos(normals[..., 2]))
    print(f"{args.out}: {args.size}x{args.size}, tilt mean {tilt.mean():.1f} deg, max {tilt.max():.1f} deg")


if __name__ == "__main__":
    main()
