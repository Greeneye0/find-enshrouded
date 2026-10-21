"""Builds annotated infographic images for the Nexus page from the in-game screenshots.
usage: python tools/make_infographics.py
Reads dist/screenshots/*.png and writes dist/nexus/info-*.png (1920x1080): the screenshot
on the left with numbered markers on each feature, the matching explanations on the right.
"""
import pathlib

from PIL import Image, ImageDraw, ImageFilter, ImageFont

root = pathlib.Path(__file__).resolve().parent.parent
shots = root / "dist" / "screenshots"
out = root / "dist" / "nexus"
FONTS = pathlib.Path(r"C:\Windows\Fonts")

GOLD = (232, 200, 130)
WHITE = (244, 240, 230)
SOFT = (200, 208, 224)
W, H = 1920, 1080


def font(name, size):
    return ImageFont.truetype(str(FONTS / name), size)


def background(shot):
    """The screenshot, blurred and darkened, as a full-frame backdrop."""
    s = max(W / shot.width, H / shot.height)
    bg = shot.resize((int(shot.width * s) + 1, int(shot.height * s) + 1), Image.LANCZOS)
    x0, y0 = (bg.width - W) // 2, (bg.height - H) // 2
    bg = bg.crop((x0, y0, x0 + W, y0 + H)).filter(ImageFilter.GaussianBlur(22)).convert("RGBA")
    bg.alpha_composite(Image.new("RGBA", (W, H), (4, 8, 22, 185)))
    return bg


def marker(d, x, y, n, r=30):
    d.ellipse([x - r - 5, y - r - 5, x + r + 5, y + r + 5], fill=(0, 0, 0, 120))
    d.ellipse([x - r, y - r, x + r, y + r], fill=GOLD + (255,), outline=(60, 40, 10, 255), width=3)
    f = font("georgiab.ttf", int(r * 1.2))
    d.text((x, y + 1), str(n), font=f, fill=(30, 22, 8, 255), anchor="mm")


def wrap(d, text, fnt, width):
    lines, line = [], ""
    for word in text.split():
        trial = (line + " " + word).strip()
        if d.textlength(trial, font=fnt) <= width:
            line = trial
        else:
            lines.append(line)
            line = word
    lines.append(line)
    return lines


def build(name, shot, crop, title, subtitle, points, notes, footer=None):
    """points: [(target x, y, marker x, y) in source pixels]; notes: [(heading, body)] in the same order."""
    pic = shot.crop(crop) if crop else shot
    img = background(pic)
    d = ImageDraw.Draw(img)

    # The screenshot, framed, on the left.
    s = min(1060 / pic.width, 960 / pic.height)
    sw, sh = int(pic.width * s), int(pic.height * s)
    px, py = 70 + (1060 - sw) // 2, (H - sh) // 2
    img.paste(pic.resize((sw, sh), Image.LANCZOS), (px, py))
    d.rectangle([px - 4, py - 4, px + sw + 3, py + sh + 3], outline=GOLD + (230,), width=3)
    ox, oy = crop[:2] if crop else (0, 0)
    def at(x, y):
        return px + (x - ox) * s, py + (y - oy) * s

    for i, (tx, ty, mx, my) in enumerate(points, 1):
        (tx, ty), (mx, my) = at(tx, ty), at(mx, my)
        d.line([(mx, my), (tx, ty)], fill=(0, 0, 0, 160), width=7)
        d.line([(mx, my), (tx, ty)], fill=GOLD + (255,), width=3)
        d.ellipse([tx - 9, ty - 9, tx + 9, ty + 9], fill=GOLD + (255,), outline=(30, 22, 8, 255), width=3)
        marker(d, mx, my, i)

    # The explanations on the right.
    left, width = 1200, 660
    d.text((left, 70), title, font=font("georgiab.ttf", 56), fill=WHITE)
    d.text((left, 145), subtitle, font=font("georgia.ttf", 30), fill=GOLD)
    d.line([(left, 197), (left + width, 197)], fill=GOLD + (200,), width=2)
    y = 232
    hf, bf = font("georgiab.ttf", 33), font("georgia.ttf", 28)
    for i, (head, body) in enumerate(notes, 1):
        marker(d, left + 26, y + 20, i, r=24)
        d.text((left + 70, y), head, font=hf, fill=WHITE)
        y += 46
        for line in wrap(d, body, bf, width - 70):
            d.text((left + 70, y), line, font=bf, fill=SOFT)
            y += 38
        y += 30
    if footer:
        ff = font("georgiai.ttf", 27)
        for line in wrap(d, footer, ff, width):
            d.text((left, y), line, font=ff, fill=GOLD)
            y += 36
    img.convert("RGB").save(out / name, optimize=True)


world = Image.open(shots / "world-chests-lit.png").convert("RGB")
search = Image.open(shots / "search-wood.png").convert("RGB")
chest = Image.open(shots / "chest-items-lit.png").convert("RGB")

build("info-world.png", world, (0, 60, world.width, 1255),
      "The chests light up", "Shift+F, type, Enter",
      [(748, 410, 560, 250), (1120, 455, 1300, 300), (1100, 615, 1320, 760)],
      [("Every chest that holds it", "All matching chests in your base pulse with the game's own gold highlight."),
       ("Even in a wall of chests", "Identical chests stacked side by side? Only the right ones glow."),
       ("Seen through walls", "The outline shows through walls and floors, so you find it on any level.")],
      "Walk over, open it, take what you need.")

build("info-search.png", search, None,
      "Search as you type", "A native-looking Enshrouded dialog",
      [(940, 335, 1180, 250), (845, 442, 1200, 430), (1056, 560, 1220, 620), (300, 893, 80, 893), (630, 1000, 630, 1150)],
      [("Partial words work", "\"wood\" finds Wood Logs, Wood Planks, Shroud Wood..."),
       ("How many, and where", "Each suggestion shows your total and how many chests hold it."),
       ("Long lists scroll", "Mouse wheel, or the arrow keys."),
       ("Find", "Lights up every chest that holds the pick."),
       ("Keyboard or mouse", "Tab / Shift+Tab or click to choose, Enter to find.")],
      "Also searches categories, rarities, item types, perks and your own chest names.")

build("info-chest.png", chest, None,
      "Then the items glow", "Inside the chest you open",
      [(125, 175, 70, 860), (600, 460, 450, 860), (713, 393, 780, 860)],
      [("Every matching stack", "Each slot holding what you searched for gets a gold glow."),
       ("All matches at once", "Searched a kind of item? All of them light up, not just one."),
       ("The rest stays as it was", "Other items keep their normal look, so the finds stand out.")],
      "The glow fades a few seconds after you open the chest.")

print("wrote info-world.png, info-search.png, info-chest.png")
