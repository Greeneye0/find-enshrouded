"""Builds the Nexus page images in dist/nexus from three in-game screenshots.
usage: python tools/make_images.py <world: chests lit> <find dialog> <opened chest>
The world shot is cropped above the hotbar and below the compass.
"""
import pathlib
import sys

from PIL import Image, ImageDraw, ImageFilter, ImageFont

root = pathlib.Path(__file__).resolve().parent.parent
out = root / "dist" / "nexus"
out.mkdir(parents=True, exist_ok=True)
world = Image.open(sys.argv[1]).convert("RGB")
dialog = Image.open(sys.argv[2]).convert("RGB")
chest = Image.open(sys.argv[3]).convert("RGB")

FONTS = pathlib.Path(r"C:\Windows\Fonts")


def font(name, size):
    return ImageFont.truetype(str(FONTS / name), size)


GOLD = (232, 200, 130)
WHITE = (244, 240, 230)
SOFT = (200, 208, 224)
NAVY = (8, 16, 36)
W, H = 1920, 1080

# The world shot without HUD: below the compass (top 70 px), above the hotbar, 16:9.
ww = world.width
wh = int(ww * 9 / 16)
top = 80
scene = world.crop((0, top, ww, top + wh)).resize((W, H), Image.LANCZOS)


def spaced(draw, xy, text, fnt, fill, spacing, anchor_right=False, center=False):
    """Draws text with extra letter spacing (the game's headings are spaced capitals)."""
    widths = [draw.textlength(c, font=fnt) for c in text]
    total = sum(widths) + spacing * (len(text) - 1)
    x, y = xy
    if anchor_right:
        x -= total
    if center:
        x -= total / 2
    for c, w in zip(text, widths):
        draw.text((x, y), c, font=fnt, fill=fill)
        x += w + spacing
    return total


def shadowed(base, draw_fn, radius=6, offset=(0, 3), alpha=170):
    """Runs draw_fn on a shadow layer (blurred, dark) and then on base."""
    layer = Image.new("RGBA", base.size, (0, 0, 0, 0))
    draw_fn(ImageDraw.Draw(layer), (0, 0, 0, alpha), offset)
    layer = layer.filter(ImageFilter.GaussianBlur(radius))
    base.alpha_composite(layer)
    draw_fn(ImageDraw.Draw(base), None, (0, 0))


def blurred(shot, w, h, darken=150):
    bg = shot.resize((w, int(shot.height * w / shot.width)), Image.LANCZOS)
    if bg.height < h:
        bg = shot.resize((int(shot.width * h / shot.height), h), Image.LANCZOS)
    x0, y0 = (bg.width - w) // 2, (bg.height - h) // 2
    bg = bg.crop((x0, y0, x0 + w, y0 + h)).filter(ImageFilter.GaussianBlur(18)).convert("RGBA")
    bg.alpha_composite(Image.new("RGBA", (w, h), (4, 8, 22, darken)))
    return bg


def framed(base, img, x0, y0):
    d = ImageDraw.Draw(base)
    d.rectangle([x0 - 4, y0 - 4, x0 + img.width + 3, y0 + img.height + 3], outline=GOLD + (220,), width=3)
    base.paste(img, (x0, y0))


def fit(img, max_w, max_h):
    s = min(max_w / img.width, max_h / img.height)
    return img.resize((int(img.width * s), int(img.height * s)), Image.LANCZOS)


def caption(base, text, y=40):
    def draw(d, shadow, off):
        f = font("georgia.ttf", 40)
        w = d.textlength(text, font=f)
        d.text(((base.width - w) / 2 + off[0], y + off[1]), text, font=f, fill=shadow or WHITE)
    shadowed(base, draw)


def right_fade(img, start, strength=210):
    w, h = img.size
    shade = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    sd = ImageDraw.Draw(shade)
    for x in range(w):
        t = max(0.0, (x - start) / (w - start))
        sd.line([(x, 0), (x, h)], fill=(4, 8, 22, int(strength * min(1.0, t * 1.6))))
    img.alpha_composite(shade)


# The lit chests close up, without the quest markers above them: chest wall and player.
close = world.crop((560, 310, world.width, 1250))

# 1. Hero: the lit chests on the left, the title on a dark panel on the right.
hero = Image.new("RGBA", (W, H), (4, 8, 22, 255))
pic = close.resize((int(close.width * H / close.height), H), Image.LANCZOS).convert("RGBA")
right_fade(pic, pic.width - 420, 255)  # the photo melts into the panel
hero.alpha_composite(pic, (0, 0))


def hero_text(d, shadow, off):
    ox, oy = off
    right = W - 90
    spaced(d, (right + ox, 300 + oy), "FIND", font("georgiab.ttf", 150), shadow or WHITE, 14, anchor_right=True)
    spaced(d, (right + ox, 480 + oy), "BASE STORAGE SEARCH", font("georgia.ttf", 40), shadow or GOLD, 5, anchor_right=True)
    if not shadow:
        d.line([(right - 520, 552), (right, 552)], fill=GOLD + (200,), width=2)
    body = font("georgia.ttf", 36)
    for i, line in enumerate(["Shift+F, type, and the chests", "that hold it light up."]):
        w = d.textlength(line, font=body)
        d.text((right - w + ox, 585 + i * 56 + oy), line, font=body, fill=shadow or SOFT)


shadowed(hero, hero_text)
hero.convert("RGB").save(out / "1-hero.png", optimize=True)

# 2. The search box.
bg = blurred(scene, W, H)
box = fit(dialog, W - 300, H - 220)
framed(bg, box, (W - box.width) // 2, (H - box.height) // 2 + 40)
caption(bg, "Suggestions as you type, with how many and where. Mouse, wheel or Tab.")
bg.convert("RGB").save(out / "2-search.png", optimize=True)

# 3. The opened chest, found items glowing.
bg = blurred(scene, W, H)
box = fit(chest, W - 300, H - 220)
framed(bg, box, (W - box.width) // 2, (H - box.height) // 2 + 40)
caption(bg, "Open a matching chest: the items you searched for glow gold")
bg.convert("RGB").save(out / "3-chest.png", optimize=True)

# 4. Feature card.
card = Image.new("RGBA", (W, H), NAVY + (255,))
cd = ImageDraw.Draw(card)
for y in range(H):
    t = y / H
    cd.line([(0, y), (W, y)], fill=(int(14 - 8 * t), int(28 - 16 * t), int(62 - 34 * t), 255))
glow = Image.new("RGBA", (W, H), (0, 0, 0, 0))
ImageDraw.Draw(glow).ellipse([W / 2 - 700, -500, W / 2 + 700, 500], fill=(70, 110, 190, 70))
card.alpha_composite(glow.filter(ImageFilter.GaussianBlur(120)))
cd = ImageDraw.Draw(card)
spaced(cd, (W / 2, 110), "SEARCH YOUR BASE", font("georgiab.ttf", 72), WHITE, 6, center=True)
cd.line([(W / 2 - 260, 222), (W / 2 + 260, 222)], fill=GOLD, width=2)
tiles = [
    ("ITEMS", "BY NAME", ["Wood Logs, Iron Arrow,", "Shroud Liquid: partial", "words work too."]),
    ("KINDS", "OF ITEMS", ["Categories, rarities,", "item types and perks:", "Consumables, Legendary."]),
    ("CHESTS", "YOU NAMED", ["Your own chest names,", "like Arrows or Food,", "light that chest up."]),
]
tile_w, tile_h, gap = 520, 450, 60
left = (W - 3 * tile_w - 2 * gap) / 2
for i, (k, sub, lines) in enumerate(tiles):
    x, y = left + i * (tile_w + gap), 310
    cd.rounded_rectangle([x, y, x + tile_w, y + tile_h], radius=18, fill=(10, 20, 44, 235), outline=GOLD + (170,), width=2)
    spaced(cd, (x + tile_w / 2, y + 60), k, font("georgiab.ttf", 50), WHITE, 4, center=True)
    spaced(cd, (x + tile_w / 2, y + 132), sub, font("georgia.ttf", 30), GOLD, 4, center=True)
    cd.line([(x + 90, y + 196), (x + tile_w - 90, y + 196)], fill=GOLD + (120,), width=1)
    bf = font("georgia.ttf", 33)
    for j, line in enumerate(lines):
        lw = cd.textlength(line, font=bf)
        cd.text((x + (tile_w - lw) / 2, y + 240 + j * 52), line, font=bf, fill=SOFT)
note = font("georgiai.ttf", 34)
for j, text in enumerate(["Follows the game's language. Client-side only: servers and friends need nothing.",
                          "Only reads your chests; never moves an item."]):
    nw = cd.textlength(text, font=note)
    cd.text(((W - nw) / 2, 840 + j * 52), text, font=note, fill=SOFT)
card.convert("RGB").save(out / "4-features.png", optimize=True)

# 0. Header banner (1300x372): the lit chests on the left, title on the right.
HW, HH = 1300, 372
band = Image.new("RGBA", (HW, HH), (4, 8, 22, 255))
pic = close.resize((int(close.width * HH * 1.5 / close.height), int(HH * 1.5)), Image.LANCZOS)
pic = pic.crop((0, int(HH * 0.12), pic.width, int(HH * 0.12) + HH)).convert("RGBA")
right_fade(pic, pic.width - 280, 255)
band.alpha_composite(pic, (0, 0))


def header_text(d, shadow, off):
    right = HW - 60
    spaced(d, (right + off[0], 78 + off[1]), "FIND", font("georgiab.ttf", 108), shadow or WHITE, 10, anchor_right=True)
    spaced(d, (right + off[0], 214 + off[1]), "BASE STORAGE SEARCH", font("georgia.ttf", 34), shadow or GOLD, 5,
           anchor_right=True)
    if not shadow:
        d.line([(right - 470, 268), (right, 268)], fill=GOLD + (200,), width=2)


shadowed(band, header_text)
band.convert("RGB").save(out / "0-header.png", optimize=True)
print("wrote", ", ".join(p.name for p in sorted(out.glob("*.png"))))
