"""
Writes the placement starter kit's meshes as OBJ files beside this script.

Every mesh is built from boxes in Unreal's own frame, in centimetres: front along +X, the object's
right along +Y, up along +Z, standing on z = 0. Triangles are wound Unreal's way round, so that
(C - A) x (B - A) points out of the front. Unreal's OBJ importer mirrors Y on the way in and keeps
the vertex order, so the file is written with Y negated and the same order, and arrives exactly as
built here.

Run it with any Python 3: python make_starter_meshes.py
"""

import os


def cross(a, b):
    return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])


def sub(a, b):
    return (a[0] - b[0], a[1] - b[1], a[2] - b[2])


def dot(a, b):
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]


class Mesh:
    def __init__(self):
        self.positions = []
        self.triangles = []

    def triangle(self, a, b, c, outward):
        # Unreal's front is the side (C - A) x (B - A) points to.
        if dot(cross(sub(c, a), sub(b, a)), outward) < 0:
            b, c = c, b
        first = len(self.positions)
        self.positions += [a, b, c]
        self.triangles.append((first, first + 1, first + 2))

    def quad(self, a, b, c, d, outward):
        self.triangle(a, b, c, outward)
        self.triangle(a, c, d, outward)

    def box(self, x0, x1, y0, y1, z0, z1):
        p = lambda x, y, z: (x, y, z)
        self.quad(p(x0, y0, z1), p(x1, y0, z1), p(x1, y1, z1), p(x0, y1, z1), (0, 0, 1))
        self.quad(p(x0, y0, z0), p(x1, y0, z0), p(x1, y1, z0), p(x0, y1, z0), (0, 0, -1))
        self.quad(p(x1, y0, z0), p(x1, y1, z0), p(x1, y1, z1), p(x1, y0, z1), (1, 0, 0))
        self.quad(p(x0, y0, z0), p(x0, y1, z0), p(x0, y1, z1), p(x0, y0, z1), (-1, 0, 0))
        self.quad(p(x0, y1, z0), p(x1, y1, z0), p(x1, y1, z1), p(x0, y1, z1), (0, 1, 0))
        self.quad(p(x0, y0, z0), p(x1, y0, z0), p(x1, y0, z1), p(x0, y0, z1), (0, -1, 0))
        return self

    def write(self, path, name):
        with open(path, 'w', newline='\n') as f:
            f.write('# Placement starter kit: %s. Written by make_starter_meshes.py.\n' % name)
            f.write('o %s\n' % name)
            for x, y, z in self.positions:
                f.write('v %.4f %.4f %.4f\n' % (x, -y, z))
            for a, b, c in self.triangles:
                f.write('f %d %d %d\n' % (a + 1, b + 1, c + 1))


def desk(depth, width, top_bottom, top, leg):
    """Four legs at the corners and a modesty panel at the back, so the knee-hole is only at the front."""
    hx, hy = depth / 2.0, width / 2.0
    m = Mesh().box(-hx, hx, -hy, hy, top_bottom, top)
    for x0 in (-hx, hx - leg):
        for y0 in (-hy, hy - leg):
            m.box(x0, x0 + leg, y0, y0 + leg, 0, top_bottom)
    m.box(-hx + 1, -hx + 3, -hy + leg, hy - leg, top_bottom - 40, top_bottom)
    return m


def chair(size, seat_bottom, seat_top, back_top, leg):
    """Legs, a seat, and a backrest on the back (-X) edge, so it faces +X."""
    h = size / 2.0
    m = Mesh().box(-h, h, -h, h, seat_bottom, seat_top)
    for x0 in (-h, h - leg):
        for y0 in (-h, h - leg):
            m.box(x0, x0 + leg, y0, y0 + leg, 0, seat_bottom)
    m.box(-h, -h + 4, -h, h, seat_top, back_top)
    return m


def shelf():
    """40 deep, 80 wide, 132 tall: side panels, a back, a plinth too low to count, and planks topped at 44, 88 and 132 cm."""
    m = Mesh()
    m.box(-20, 20, -40, -38, 0, 132)
    m.box(-20, 20, 38, 40, 0, 132)
    m.box(-20, -19, -38, 38, 6, 129)
    m.box(-20, 20, -38, 38, 0, 6)
    for top in (44, 88, 132):
        m.box(-20, 20, -38, 38, top - 3, top)
    return m


def lamp():
    return Mesh().box(-8, 8, -8, 8, 0, 2).box(-1.5, 1.5, -1.5, 1.5, 2, 28).box(-9, 9, -9, 9, 28, 38)


MESHES = {
    'SM_Desk_Oak': desk(60, 120, 71, 75, 5),
    'SM_Desk_Steel': desk(80, 140, 72, 75, 4),
    'SM_Chair_Oak': chair(44, 42, 46, 88, 3),
    'SM_Chair_Steel': chair(48, 44, 48, 96, 3),
    'SM_Crate_Small': Mesh().box(-19, 19, -19, 19, 0, 38),
    'SM_Crate_Long': Mesh().box(-19, 19, -39, 39, 0, 38),
    'SM_Shelf': shelf(),
    'SM_Lamp': lamp(),
    'SM_Books': Mesh().box(-8, 8, -14, 14, 0, 18),
}

if __name__ == '__main__':
    here = os.path.dirname(os.path.abspath(__file__))
    for name, mesh in MESHES.items():
        mesh.write(os.path.join(here, name + '.obj'), name)
        print('wrote', name)
