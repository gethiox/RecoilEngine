#!/usr/bin/env python3
# This file is part of the Spring engine (GPL v2 or later), see LICENSE.html
"""
Generate a small self-contained SMF test map (as an .sdd directory) so that
snapshot-join integrity tests can run without downloading content.

The map is fully deterministic (fixed pseudo-random seed) and contains:
  - rolling hills and a few steep plateaus (pathing variety),
  - a lake deep enough for ships / submarines / hovercraft,
  - a symmetric set of metal spots and geothermal-free flat start areas.

Usage: gen_test_map.py <output-dir> [--size 12]
Produces <output-dir>/SnapshotTestMap.sdd/{mapinfo.lua,maps/*.smf,maps/*.smt}
"""

import argparse
import math
import os
import random
import struct

MAP_NAME = "SnapshotTestMap"
TILE_PX = 32
SMALL_TILE_SIZE = 512 + 128 + 32 + 8
MINIMAP_SIZE = 699048


def rgb565(r, g, b):
	return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)


def dxt1_solid_block(r, g, b):
	c = rgb565(r, g, b)
	# color0 == color1 -> 3-color mode, index 0 everywhere selects color0.
	return struct.pack("<HHI", c, c, 0)


def dxt1_solid_image(w, h, rgb):
	block = dxt1_solid_block(*rgb)
	return block * (max(1, w // 4) * max(1, h // 4))


def dxt1_solid_tile(rgb):
	data = b""
	for size in (32, 16, 8, 4):
		data += dxt1_solid_image(size, size, rgb)
	assert len(data) == SMALL_TILE_SIZE
	return data


def build_heightmap(mapx, mapy, rnd):
	hx, hy = mapx + 1, mapy + 1
	# a few gaussian hills, one plateau per side, and a central lake
	hills = [(rnd.uniform(0.1, 0.9) * mapx, rnd.uniform(0.1, 0.9) * mapy,
	          rnd.uniform(20, 60), rnd.uniform(60, 140)) for _ in range(14)]
	plateaus = [(0.30 * mapx, 0.25 * mapy), (0.70 * mapx, 0.75 * mapy)]
	cx, cy = 0.5 * mapx, 0.5 * mapy
	lake_r = 0.13 * min(mapx, mapy)

	heights = [0.0] * (hx * hy)
	for y in range(hy):
		for x in range(hx):
			h = 100.0
			for (px, py, r, a) in hills:
				d2 = ((x - px) ** 2 + (y - py) ** 2) / (r * r)
				h += a * math.exp(-d2)
			for (px, py) in plateaus:
				d = math.hypot(x - px, y - py)
				# steep-sided plateau with ramps along the x axis
				if d < 28:
					ramp = abs(y - py) < 6
					h += 120.0 if not ramp else 120.0 * max(0.0, 1.0 - abs(x - px) / 28.0)
				elif d < 34 and abs(y - py) >= 6:
					h += 120.0 * (34 - d) / 6.0
			d = math.hypot(x - cx, y - cy)
			if d < lake_r:
				h -= 200.0 * (1.0 - (d / lake_r) ** 2)
			heights[y * hx + x] = h
	return heights


def metal_spots(mapx, mapy):
	spots = []
	for (fx, fy) in [(0.12, 0.12), (0.2, 0.08), (0.08, 0.2), (0.35, 0.45), (0.45, 0.2),
	                 (0.25, 0.6), (0.6, 0.3), (0.15, 0.85), (0.5, 0.5)]:
		spots.append((fx, fy))
		spots.append((1.0 - fx, 1.0 - fy))
	return [(int(fx * mapx / 2), int(fy * mapy / 2)) for (fx, fy) in spots]


def write_map(outdir, size):
	rnd = random.Random(0x5EED)
	mapx = mapy = size * 64
	sdd = os.path.join(outdir, MAP_NAME + ".sdd")
	os.makedirs(os.path.join(sdd, "maps"), exist_ok=True)

	heights = build_heightmap(mapx, mapy, rnd)
	min_h, max_h = min(heights), max(heights)
	hm = [int(round((h - min_h) / (max_h - min_h) * 0xFFFF)) for h in heights]

	# tiles: palette by height band (sand, grass, dark grass, rock, snow)
	palette = [(194, 178, 128), (96, 140, 70), (70, 110, 55), (120, 110, 100), (220, 220, 225)]
	tiles = [dxt1_solid_tile(c) for c in palette]
	tx, ty = mapx // 4, mapy // 4
	tile_idx = []
	for y in range(ty):
		for x in range(tx):
			h = heights[(y * 4 + 2) * (mapx + 1) + (x * 4 + 2)]
			t = 0 if h < 0 else (1 if h < 150 else (2 if h < 210 else (3 if h < 260 else 4)))
			tile_idx.append(t)

	metal = bytearray((mapx // 2) * (mapy // 2))
	for (sx, sy) in metal_spots(mapx, mapy):
		for dy in range(-3, 4):
			for dx in range(-3, 4):
				if dx * dx + dy * dy <= 9:
					x, y = sx + dx, sy + dy
					if 0 <= x < mapx // 2 and 0 <= y < mapy // 2:
						metal[y * (mapx // 2) + x] = 255

	smt_name = MAP_NAME + ".smt"
	header_size = 16 + 4 * 16
	body = bytearray()

	def offset():
		return header_size + len(body)

	heightmap_ptr = offset()
	body += struct.pack("<%dH" % len(hm), *hm)
	type_ptr = offset()
	body += bytes((mapx // 2) * (mapy // 2))
	minimap_ptr = offset()
	mm = b""
	size_px = 1024
	for _ in range(9):
		mm += dxt1_solid_image(size_px, size_px, palette[1])
		size_px //= 2
	mm = mm[:MINIMAP_SIZE].ljust(MINIMAP_SIZE, b"\0")
	body += mm
	metal_ptr = offset()
	body += metal
	feature_ptr = offset()
	body += struct.pack("<ii", 0, 0)
	tiles_ptr = offset()
	body += struct.pack("<ii", 1, len(tiles))
	body += struct.pack("<i", len(tiles)) + smt_name.encode() + b"\0"
	body += struct.pack("<%di" % len(tile_idx), *tile_idx)

	header = b"spring map file\0" + struct.pack(
		"<iiiiiiiffiiiiiii", 1, 0x5EED, mapx, mapy, 8, 8, 32, min_h, max_h,
		heightmap_ptr, type_ptr, tiles_ptr, minimap_ptr, metal_ptr, feature_ptr, 0)
	assert len(header) == header_size

	with open(os.path.join(sdd, "maps", MAP_NAME + ".smf"), "wb") as f:
		f.write(header + body)
	with open(os.path.join(sdd, "maps", smt_name), "wb") as f:
		f.write(b"spring tilefile\0" + struct.pack("<iiii", 1, len(tiles), 32, 1))
		for t in tiles:
			f.write(t)

	el = mapx * 8
	starts = [(0.12, 0.12), (0.88, 0.88), (0.88, 0.12), (0.12, 0.88)]
	teams = ",\n\t\t".join("[%d] = {startPos = {x = %d, z = %d}}" % (i, int(fx * el), int(fy * el))
	                       for i, (fx, fy) in enumerate(starts))
	with open(os.path.join(sdd, "mapinfo.lua"), "w") as f:
		f.write("""local mapinfo = {
	name        = "%(name)s",
	shortname   = "SnapTest",
	description = "Generated snapshot-join test map",
	author      = "gen_test_map.py",
	version     = "",
	mapfile     = "maps/%(name)s.smf",
	modtype     = 3,
	depend      = {},
	replace     = {},
	maphardness     = 100,
	notDeformable   = false,
	gravity         = 130,
	tidalStrength   = 15,
	maxMetal        = 0.02,
	extractorRadius = 90.0,
	voidWater       = false,
	smf = {
		minheight = %(minh)f,
		maxheight = %(maxh)f,
	},
	atmosphere = {
		minWind = 5.0,
		maxWind = 25.0,
	},
	teams = {
		%(teams)s
	},
	terrainTypes = {
		[0] = {
			name = "Default",
			hardness = 1.0,
			receiveTracks = true,
			moveSpeeds = { tank = 1.0, kbot = 1.0, hover = 1.0, ship = 1.0 },
		},
	},
}
return mapinfo
""" % {"name": MAP_NAME, "minh": min_h, "maxh": max_h, "teams": teams})
	return sdd


def main():
	ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
	ap.add_argument("outdir")
	ap.add_argument("--size", type=int, default=12, help="map size in standard map units (64 squares each)")
	args = ap.parse_args()
	print(write_map(args.outdir, args.size))


if __name__ == "__main__":
	main()
