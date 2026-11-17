#!/usr/bin/env python3
"""Make the sample models of the OpenGFX3D package.

The models are simple shapes written straight to binary glTF files, so they need no modelling program:

    python3 make_models.py

Shapes are described with x towards the front of the vehicle, y to its left and z up, in world units
(a tile is 16 long), with the ground at z = 0 and the middle of the vehicle at x = y = 0.
"""

import json
import math
import struct

# Colours as seen on screen, 0 to 1. "company" is multiplied with the colour of the owner.
MATERIALS = {
    "company": (1.00, 1.00, 1.00),
    "company_dark": (0.72, 0.72, 0.72),
    "glass": (0.16, 0.22, 0.30),
    "tyre": (0.10, 0.10, 0.10),
    "hub": (0.60, 0.60, 0.60),
    "dark": (0.22, 0.22, 0.24),
    "roof": (0.86, 0.86, 0.84),
    "light": (1.00, 0.95, 0.70),
    "cargo": (0.70, 0.71, 0.74),
    "cargo_dark": (0.50, 0.51, 0.54),
}


class Model:
    def __init__(self):
        self.parts = {}  # material -> (positions, normals), three corners per triangle

    def triangle(self, material, a, b, c):
        u = [b[i] - a[i] for i in range(3)]
        v = [c[i] - a[i] for i in range(3)]
        n = [u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0]]
        length = math.sqrt(sum(x * x for x in n)) or 1.0
        n = [x / length for x in n]
        positions, normals = self.parts.setdefault(material, ([], []))
        positions.extend([a, b, c])
        normals.extend([n, n, n])

    def quad(self, material, a, b, c, d):
        """Four corners, counter-clockwise when seen from outside."""
        self.triangle(material, a, b, c)
        self.triangle(material, a, c, d)

    def box(self, material, x0, x1, y0, y1, z0, z1):
        self.quad(material, (x1, y0, z0), (x1, y1, z0), (x1, y1, z1), (x1, y0, z1))  # front
        self.quad(material, (x0, y1, z0), (x0, y0, z0), (x0, y0, z1), (x0, y1, z1))  # back
        self.quad(material, (x1, y1, z0), (x0, y1, z0), (x0, y1, z1), (x1, y1, z1))  # left
        self.quad(material, (x0, y0, z0), (x1, y0, z0), (x1, y0, z1), (x0, y0, z1))  # right
        self.quad(material, (x0, y0, z1), (x1, y0, z1), (x1, y1, z1), (x0, y1, z1))  # top
        self.quad(material, (x0, y1, z0), (x1, y1, z0), (x1, y0, z0), (x0, y0, z0))  # bottom

    def wheel(self, x, y, radius, width, sides=12):
        """A wheel with its axle along y, standing on the ground."""
        y0, y1 = y - width / 2, y + width / 2
        ring = [(x + radius * math.cos(2 * math.pi * i / sides), radius + radius * math.sin(2 * math.pi * i / sides)) for i in range(sides)]
        for i in range(sides):
            (ax, az), (bx, bz) = ring[i], ring[(i + 1) % sides]
            self.quad("tyre", (ax, y1, az), (ax, y0, az), (bx, y0, bz), (bx, y1, bz))
            self.triangle("hub", (x, y1, radius), (ax, y1, az), (bx, y1, bz))
            self.triangle("hub", (x, y0, radius), (bx, y0, bz), (ax, y0, az))

    def save(self, filename):
        binary = bytearray()
        views, accessors, materials, primitives = [], [], [], []

        def add(vectors):
            # glTF has x to the left, y up and z to the front.
            data = b"".join(struct.pack("<3f", v[1], v[2], v[0]) for v in vectors)
            views.append({"buffer": 0, "byteOffset": len(binary), "byteLength": len(data)})
            binary.extend(data)
            converted = [(v[1], v[2], v[0]) for v in vectors]
            accessors.append({
                "bufferView": len(views) - 1, "componentType": 5126, "count": len(vectors), "type": "VEC3",
                "min": [min(v[i] for v in converted) for i in range(3)],
                "max": [max(v[i] for v in converted) for i in range(3)],
            })
            return len(accessors) - 1

        for name, (positions, normals) in self.parts.items():
            # glTF stores colours as amounts of light, not as seen on screen.
            colour = [round(c ** 2.2, 4) for c in MATERIALS[name]] + [1.0]
            materials.append({"name": name, "pbrMetallicRoughness": {"baseColorFactor": colour, "metallicFactor": 0.0, "roughnessFactor": 0.8}})
            primitives.append({"attributes": {"POSITION": add(positions), "NORMAL": add(normals)}, "material": len(materials) - 1})

        scene = {
            "asset": {"version": "2.0", "generator": "OpenGFX3D make_models.py"},
            "scene": 0,
            "scenes": [{"nodes": [0]}],
            "nodes": [{"mesh": 0}],
            "meshes": [{"primitives": primitives}],
            "materials": materials,
            "accessors": accessors,
            "bufferViews": views,
            "buffers": [{"byteLength": len(binary)}],
        }
        text = json.dumps(scene, separators=(",", ":")).encode()
        text += b" " * (-len(text) % 4)
        binary += b"\0" * (-len(binary) % 4)
        with open(filename, "wb") as f:
            f.write(struct.pack("<III", 0x46546C67, 2, 12 + 8 + len(text) + 8 + len(binary)))
            f.write(struct.pack("<II", len(text), 0x4E4F534A) + text)
            f.write(struct.pack("<II", len(binary), 0x004E4942) + bytes(binary))


def bus():
    m = Model()
    length, width = 9.0, 3.2
    x0, x1, y0, y1 = -length / 2, length / 2, -width / 2, width / 2
    m.box("dark", x0 + 0.1, x1 - 0.1, y0 + 0.15, y1 - 0.15, 0.45, 0.8)  # underside
    m.box("company", x0, x1, y0, y1, 0.8, 2.0)  # lower body
    m.box("glass", x0 + 0.08, x1 - 0.08, y0 + 0.08, y1 - 0.08, 2.0, 3.0)  # windows
    m.box("roof", x0, x1, y0, y1, 3.0, 3.3)
    m.box("roof", x0 + 0.5, x1 - 0.5, y0 + 0.4, y1 - 0.4, 3.3, 3.42)
    # Pillars between the windows, and the corners.
    for i in range(7):
        x = x0 + i * (length - 0.3) / 6
        for y in (y0, y1 - 0.12):
            m.box("company", x, x + 0.3, y, y + 0.12, 2.0, 3.0)
    for y in (y0, y1 - 0.3):
        m.box("company", x0, x0 + 0.12, y, y + 0.3, 2.0, 3.0)
        m.box("company", x1 - 0.12, x1, y, y + 0.3, 2.0, 3.0)
    m.box("company_dark", x0, x0 + 0.12, y0, y1, 2.0, 2.3)  # below the rear window
    # Bumpers and lights.
    m.box("dark", x1, x1 + 0.15, y0, y1, 0.7, 1.05)
    m.box("dark", x0 - 0.15, x0, y0, y1, 0.7, 1.05)
    for y in (y0 + 0.2, y1 - 0.7):
        m.box("light", x1, x1 + 0.06, y, y + 0.5, 1.2, 1.55)
    for x in (-2.7, 2.7):
        for y in (y0 + 0.2, y1 - 0.2):
            m.wheel(x, y, 0.62, 0.45)
    m.save("bus.glb")


def lorry():
    m = Model()
    length, width = 9.0, 3.1
    x0, x1, y0, y1 = -length / 2, length / 2, -width / 2, width / 2
    m.box("dark", x0, x1 - 0.1, y0 + 0.3, y1 - 0.3, 0.55, 1.0)  # chassis
    # Cab.
    m.box("company", x1 - 2.2, x1, y0, y1, 0.8, 1.9)
    m.box("glass", x1 - 2.1, x1 - 0.06, y0 + 0.06, y1 - 0.06, 1.9, 2.75)
    m.box("company", x1 - 2.2, x1, y0, y1, 2.75, 2.95)
    m.box("company", x1 - 2.2, x1 - 1.9, y0, y1, 1.9, 2.75)  # back of the cab
    for y in (y0, y1 - 0.14):
        m.box("company", x1 - 0.14, x1, y, y + 0.14, 1.9, 2.75)
    m.box("dark", x1, x1 + 0.15, y0, y1, 0.7, 1.05)
    for y in (y0 + 0.2, y1 - 0.7):
        m.box("light", x1, x1 + 0.06, y, y + 0.5, 1.2, 1.5)
    # Load.
    m.box("cargo", x0, x1 - 2.5, y0, y1, 1.0, 3.3)
    m.box("cargo_dark", x0 - 0.04, x0, y0 + 0.2, y1 - 0.2, 1.2, 3.1)  # doors
    for x in (-3.0, -1.6, 3.0):
        for y in (y0 + 0.2, y1 - 0.2):
            m.wheel(x, y, 0.62, 0.45)
    m.save("lorry.glb")


if __name__ == "__main__":
    bus()
    lorry()
