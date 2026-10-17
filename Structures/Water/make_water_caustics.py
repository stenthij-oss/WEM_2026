"""
Makes the water's caustics: the web of bright lines sunlight draws through a rippled surface, as a
seamless tile, so the water material can repeat it across the world with no seam.

Light comes straight down through a periodic height field. Each ray is bent by the surface's slope,
lands a little way off, and is counted where it lands, wrapping round the tile's edges. Where rays
crowd together the light gathers into lines; where they spread apart it is dim. Both the surface and
the landing wrap, so the tile carries on into its other side exactly.

    python make_water_caustics.py [out.png] [--size 1024] [--seed 11] [--peak 9] [--width 3] [--bend 12] [--gamma 1.0] [--bright 98.5]

Writes an 8-bit greyscale map, bright on the lines. Import it with sRGB off.
"""

import argparse

import numpy as np
from PIL import Image


def make_swell(size: int, seed: int, peak: float, width: float) -> np.ndarray:
    """
    A smooth periodic surface: waves every way alike, all near Peak per tile. Caustics gather where the
    surface curves, which favours fine ripples far more than its lean does, so anything much finer than
    the peak is left out, or it breaks the web into grain.
    """
    rng = np.random.default_rng(seed)

    k = np.fft.fftfreq(size, d=1.0 / size)
    kx, ky = np.meshgrid(k, k)
    radius = np.hypot(kx, ky)

    spectrum = np.exp(-(((radius - peak) / width) ** 2))
    spectrum[radius < 1.0] = 0.0

    noise = rng.normal(size=(size, size)) + 1j * rng.normal(size=(size, size))
    height = np.real(np.fft.ifft2(noise * np.sqrt(spectrum)))
    return height / np.abs(height).max()


def blur(image: np.ndarray, radius: float) -> np.ndarray:
    """A Gaussian blur that wraps round the tile, done in frequency space."""
    size = image.shape[0]
    k = np.fft.fftfreq(size)
    kx, ky = np.meshgrid(k, k)
    return np.real(np.fft.ifft2(np.fft.fft2(image) * np.exp(-2.0 * (np.pi * radius) ** 2 * (kx * kx + ky * ky))))


def make_caustics(size: int, seed: int, peak: float, width: float, bend: float, rays: int) -> np.ndarray:
    height = make_swell(size * rays, seed, peak, width)

    # The slope at every ray, in cells of the finer grid, wrapping round the tile.
    du = (np.roll(height, -1, axis=1) - np.roll(height, 1, axis=1)) * 0.5
    dv = (np.roll(height, -1, axis=0) - np.roll(height, 1, axis=0)) * 0.5
    slope = np.hypot(du, dv).mean()

    # Each ray lands off its start against the slope, by Bend cells of the output on an average slope.
    fine = size * rays
    ys, xs = np.mgrid[0:fine, 0:fine].astype(np.float64)
    x = (xs - du / slope * bend * rays) / rays
    y = (ys - dv / slope * bend * rays) / rays

    # Counted into the four cells round where it lands, by how near it is to each.
    x0 = np.floor(x).astype(np.int64)
    y0 = np.floor(y).astype(np.int64)
    fx = x - x0
    fy = y - y0
    light = np.zeros(size * size)

    for ox, oy, weight in ((0, 0, (1 - fx) * (1 - fy)), (1, 0, fx * (1 - fy)), (0, 1, (1 - fx) * fy), (1, 1, fx * fy)):
        cells = ((y0 + oy) % size) * size + (x0 + ox) % size
        np.add.at(light, cells.ravel(), weight.ravel())

    # A touch of blur, as light through real water never comes to a perfect line, and to keep the counting smooth.
    return blur(light.reshape(size, size) / (rays * rays), 0.8)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("out", nargs="?", default="T_Water_Caustics.png")
    parser.add_argument("--size", type=int, default=1024)
    parser.add_argument("--seed", type=int, default=11)
    parser.add_argument("--peak", type=float, default=9.0, help="waves per tile the light comes through")
    parser.add_argument("--width", type=float, default=3.0, help="how far either side of the peak they reach")
    parser.add_argument("--bend", type=float, default=12.0, help="how far rays land off, in output pixels")
    parser.add_argument("--rays", type=int, default=3, help="rays per output pixel along each side")
    parser.add_argument("--gamma", type=float, default=1.0, help="above 1 thins the lines")
    parser.add_argument("--bright", type=float, default=98.5, help="percentile of the light shown as white")
    args = parser.parse_args()

    light = make_caustics(args.size, args.seed, args.peak, args.width, args.bend, args.rays)

    # Level light is 1. Shown from the dimmest to the brightest part of the lines, the knots where they cross
    # past white, then bent so the lines stay thin.
    low, high = np.percentile(light, 1), np.percentile(light, args.bright)
    shown = np.clip((light - low) / (high - low), 0, 1) ** args.gamma
    Image.fromarray((shown * 255 + 0.5).astype(np.uint8), mode="L").save(args.out)

    print(f"{args.out}: {args.size}x{args.size}, light {light.min():.2f}..{light.max():.2f}, mean shown {shown.mean():.3f}")


if __name__ == "__main__":
    main()
