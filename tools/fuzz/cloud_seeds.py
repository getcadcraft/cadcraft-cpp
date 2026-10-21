#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Seeds for the `cloud` fuzz target: tiny synthetic point clouds (a 10 x 10 x 3 grid of points), ours. LAS 1.2 format 0
# and 3, LAS 1.4 format 7 and 8, LAZ of formats 3 and 7, an XYZ and a PTS. Needs laspy + lazrs (the point cloud
# corpus venv: testfiles/pointcloud-corpus/venv). Run once; the files are committed.
#   python3 tools/fuzz/cloud_seeds.py fuzz/corpus/cloud
import os, sys
import numpy as np
import laspy

out = sys.argv[1]
os.makedirs(out, exist_ok=True)
g = np.array([(x * 0.5, y * 0.5, z * 1.0) for x in range(10) for y in range(10) for z in range(3)])
n = len(g)
k = np.arange(n)
for name, version, fmt, laz in [('tiny-f0.las', '1.2', 0, False), ('tiny-f3.las', '1.2', 3, False),
                                ('tiny-f7.las', '1.4', 7, False), ('tiny-f8.las', '1.4', 8, False),
                                ('tiny-f3.laz', '1.2', 3, True), ('tiny-f7.laz', '1.4', 7, True)]:
    h = laspy.LasHeader(point_format=fmt, version=version)
    h.scales = np.array([0.001, 0.001, 0.001])
    h.offsets = np.array([1000.0, 2000.0, 0.0])
    d = laspy.LasData(h)
    d.x, d.y, d.z = g[:, 0] + 1000.0, g[:, 1] + 2000.0, g[:, 2]
    d.classification = (k % 7).astype(np.uint8)
    d.intensity = (k * 37 % 4000).astype(np.uint16)
    if 'red' in d.point_format.dimension_names:
        d.red, d.green, d.blue = (k * 257 % 65535), (k * 511 % 65535), (k * 1021 % 65535)
    path = os.path.join(out, name)
    d.write(path, laz_backend=laspy.LazBackend.Lazrs) if laz else d.write(path)
with open(os.path.join(out, 'tiny.xyz'), 'w') as f:
    for p in g:
        f.write('%.3f %.3f %.3f %d %d %d\n' % (p[0], p[1], p[2], 10, 20, 30))
with open(os.path.join(out, 'tiny.pts'), 'w') as f:
    f.write('%d\n' % n)
    for i, p in enumerate(g):
        f.write('%.3f,%.3f,%.3f,%d,%d,%d,%d\n' % (p[0], p[1], p[2], i % 255, 1, 2, 3))
