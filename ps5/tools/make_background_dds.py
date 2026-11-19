#!/usr/bin/env python3
"""The app's home screen background for the PS5: sce_sys/pic0.dds and pic1.dds.

The PS5 menu takes the background as a 3840x2160 DDS in BC7 (one mip level,
a DX10 header), as the GTA IV PS5 port ships it; PNGs are not picked up.

usage: make_background_dds.py <picture> <out.dds> <preview.jpg> [anchor_y]

The picture is scaled by the same amount both ways (never stretched) until it
covers 3840x2160, and what is left over is cut off: anchor_y says where from
(0 keeps the top, 1 the bottom, 0.5 cuts evenly; the same for the sides when
the picture is wider than 16:9). preview.jpg is the DDS decoded again, at
1280x720 (the README's picture).
Needs: pip install etcpak texture2ddecoder pillow
"""
import struct
import sys

import etcpak
import texture2ddecoder
from PIL import Image

W, H = 3840, 2160

src, out, preview = sys.argv[1], sys.argv[2], sys.argv[3]
anchor = float(sys.argv[4]) if len(sys.argv) > 4 else 0.5

im = Image.open(src).convert('RGB')
w, h = im.size
scale = max(W / w, H / h)
sw, sh = round(w * scale), round(h * scale)
im = im.resize((sw, sh), Image.LANCZOS)
left = round((sw - W) * anchor)
top = round((sh - H) * anchor)
im = im.crop((left, top, left + W, top + H)).convert('RGBA')
print(f'{w}x{h} scaled by {scale:.3f} to {sw}x{sh}, cut to {W}x{H} at ({left}, {top})')

data = etcpak.compress_bc7(im.tobytes('raw', 'RGBA'), W, H)
assert len(data) == W * H, len(data)
# Decoded again (texture2ddecoder gives BGRA): to look at, and to check.
back = Image.frombytes('RGBA', (W, H), texture2ddecoder.decode_bc7(data, W, H), 'raw', 'BGRA')
points = [(x, y) for x in range(100, W, 700) for y in range(100, H, 500)]
err = sum(abs(a - b) for p in points for a, b in zip(im.getpixel(p)[:3], back.getpixel(p)[:3]))
print('mean difference at the sampled pixels:', round(err / len(points), 1))
back.convert('RGB').resize((1280, 720), Image.LANCZOS).save(preview, quality=88)

header = struct.pack('<4s31I', b'DDS ', 124, 0x81007, H, W, W * H, 0, 1, *([0] * 11),
                     32, 4, struct.unpack('<I', b'DX10')[0], 0, 0, 0, 0, 0, 0x1000, 0, 0, 0, 0)
dx10 = struct.pack('<5I', 98, 3, 0, 1, 0)  # BC7_UNORM, 2D, one image
with open(out, 'wb') as f:
    f.write(header + dx10 + data)
