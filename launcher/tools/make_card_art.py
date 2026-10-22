"""Generates the launcher's original card artwork (launcher/romfs/ui/card-*.png).

Run from launcher/: python tools/make_card_art.py. Needs Pillow and romfs/fonts/MaterialIconsRound.otf.
"""
import math

from PIL import Image, ImageDraw, ImageFilter, ImageFont

S = 2
W, H = 520 * S, 330 * S
ICONS = "romfs/fonts/MaterialIconsRound.otf"
NAVY = (38, 52, 78, 255)


def base(top, bot, check):
    im = Image.new("RGBA", (W, H))
    d = ImageDraw.Draw(im)
    for y in range(H):
        t = y / H
        d.line([(0, y), (W, y)], fill=tuple(int(top[i] + (bot[i] - top[i]) * t) for i in range(3)) + (255,))
    ov = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    od = ImageDraw.Draw(ov)
    s = 36 * S
    for gy in range(0, H, s):
        for gx in range(0, W, s):
            if (gx // s + gy // s) % 2 == 0:
                od.rectangle([gx, gy, gx + s - 1, gy + s - 1], fill=check + (int(44 * gx / W),))
    im = Image.alpha_composite(im, ov)
    wv = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    wd = ImageDraw.Draw(wv)
    for base_y, amp, phase, alpha in ((70, 18, 0.6, 150), (46, 14, 2.2, 215)):
        pts = [(x, H - base_y * S + amp * S * math.sin(x / W * math.pi * 2 + phase)) for x in range(0, W + 1, 4)]
        wd.polygon(pts + [(W, H), (0, H)], fill=(255, 255, 255, alpha))
    return Image.alpha_composite(im, wv)


def glyph(im, cp, cx, cy, color, size, mirror=False):
    f = ImageFont.truetype(ICONS, size)
    g = Image.new("RGBA", (size * 2, size * 2), (0, 0, 0, 0))
    ImageDraw.Draw(g).text((size, size), chr(cp), font=f, fill=color, anchor="mm")
    if mirror:
        g = g.transpose(Image.FLIP_LEFT_RIGHT)
    sh = Image.composite(Image.new("RGBA", g.size, (20, 40, 80, 90)), Image.new("RGBA", g.size, (0, 0, 0, 0)),
                         g.split()[3]).filter(ImageFilter.GaussianBlur(6 * S))
    im.alpha_composite(sh, (int(cx - size), int(cy - size + 8 * S)))
    im.alpha_composite(g, (int(cx - size), int(cy - size)))


def finish(im, path):
    im = im.resize((W // S, H // S), Image.LANCZOS)
    w, h = im.size
    m = Image.new("L", (w * 4, h * 4), 0)
    ImageDraw.Draw(m).rounded_rectangle([0, 0, w * 4 - 1, h * 4 - 1], radius=18 * 4, fill=255)
    im.putalpha(m.resize((w, h), Image.LANCZOS))
    im.save(path)


# Vanilla: steering wheel between two checkered flags.
im = base((214, 234, 255), (140, 195, 250), (40, 90, 170))
cx, cy = W // 2, H // 2 - 22 * S
glyph(im, 0xF06E, cx - 160 * S, cy + 5 * S, NAVY, 130 * S, mirror=True)
glyph(im, 0xF06E, cx + 160 * S, cy + 5 * S, NAVY, 130 * S)
R = 100 * S
sh = Image.new("RGBA", (W, H), (0, 0, 0, 0))
ImageDraw.Draw(sh).ellipse([cx - R, cy - R + 10 * S, cx + R, cy + R + 10 * S], fill=(20, 40, 80, 70))
im.alpha_composite(sh.filter(ImageFilter.GaussianBlur(8 * S)))
d = ImageDraw.Draw(im)
d.ellipse([cx - R, cy - R, cx + R, cy + R], fill=NAVY)
r2 = R - 25 * S
d.ellipse([cx - r2, cy - r2, cx + r2, cy + r2], fill=(235, 243, 252, 255))
for a in (90, 210, 330):
    d.line([(cx, cy), (cx + math.cos(math.radians(a)) * r2, cy + math.sin(math.radians(a)) * r2)], fill=NAVY, width=30 * S)
d.ellipse([cx - 38 * S, cy - 38 * S, cx + 38 * S, cy + 38 * S], fill=NAVY)
d.ellipse([cx - 20 * S, cy - 20 * S, cx + 20 * S, cy + 20 * S], fill=(47, 139, 245, 255))
finish(im, "romfs/ui/card-vanilla.png")

# Retro Rewind: a rewind clock.
im = base((222, 247, 226), (150, 218, 160), (40, 130, 60))
glyph(im, 0xE889, W // 2, H // 2 - 26 * S, NAVY, 220 * S)
glyph(im, 0xE020, W // 2 + 110 * S, H // 2 + 40 * S, (67, 176, 90, 255), 100 * S)
finish(im, "romfs/ui/card-rr.png")
print("ok")
