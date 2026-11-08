# Find 1.2.11 preview card (dist/nexus/1-hero.png): a vertical split, lit chests | lit skill tree.
import pathlib
from PIL import Image, ImageDraw, ImageFilter, ImageFont

root = pathlib.Path(__file__).resolve().parents[1]
shots = root / "dist/screenshots"
W, H, HALF = 1920, 1080, 960
GOLD, CREAM, INK = (232, 200, 130), (244, 238, 224), (10, 12, 26)
FONT = "C:/Windows/Fonts/georgiab.ttf"


def fill(img, box, size):
    return img.crop(box).resize(size, Image.LANCZOS)


chests = Image.open(shots / "world-chests-lit.png").convert("RGB")
skills = Image.open(shots / "skills-lit-heal.png").convert("RGB")
card = Image.new("RGB", (W, H))
card.paste(fill(chests, (560, 150, 1520, 1230), (HALF, H)), (0, 0))
card.paste(fill(skills, (1100, 120, 2273, 1440), (HALF, H)), (HALF, 0))

# Darken the top for the wordmark and the bottom for the labels.
shade = Image.new("L", (W, H), 0)
d = ImageDraw.Draw(shade)
for y in range(H):
    top = max(0, 1 - y / 330) * 205
    bottom = max(0, (y - (H - 230)) / 230) * 190
    d.line([(0, y), (W, y)], fill=int(max(top, bottom)))
card = Image.composite(Image.new("RGB", (W, H), INK), card, shade)
draw = ImageDraw.Draw(card)

# Divider: a gold rule with a soft glow.
glow = Image.new("RGB", (W, H))
ImageDraw.Draw(glow).rectangle([HALF - 6, 0, HALF + 6, H], fill=GOLD)
card = Image.blend(card, Image.composite(glow, card, glow.convert("L").filter(ImageFilter.GaussianBlur(10))), 0.55)
draw = ImageDraw.Draw(card)
draw.rectangle([HALF - 2, 0, HALF + 1, H], fill=GOLD)


def centred(text, font, y, fill, spacing=0, x=W // 2):
    w = sum(draw.textlength(c, font=font) for c in text) + spacing * (len(text) - 1)
    cx = x - w / 2
    for c in text:
        draw.text((cx, y), c, font=font, fill=fill)
        cx += draw.textlength(c, font=font) + spacing


big = ImageFont.truetype(FONT, 150)
sub = ImageFont.truetype("C:/Windows/Fonts/georgia.ttf", 34)
label = ImageFont.truetype(FONT, 40)
note = ImageFont.truetype("C:/Windows/Fonts/georgia.ttf", 28)

# Wordmark on a dark plate over the divider.
plate = [W // 2 - 300, 34, W // 2 + 300, 252]
draw.rounded_rectangle(plate, 14, fill=INK, outline=GOLD, width=3)
centred("FIND", big, 44, CREAM, spacing=14)
centred("BASE STORAGE  ·  SKILL TREE", sub, 204, GOLD, spacing=3)

# Labels, one per half.
for x, title, line in ((HALF // 2, "Your chests", "Shift+F in your base"), (HALF + HALF // 2, "Your skills", "Shift+F on the Skills page")):
    draw.rounded_rectangle([x - 250, H - 170, x + 250, H - 40], 12, fill=(38, 26, 16), outline=GOLD, width=3)
    centred(title, label, H - 158, CREAM, x=x)
    centred(line, note, H - 96, GOLD, x=x)

card.save(root / "dist/nexus/1-hero.png", optimize=True)
print("saved", card.size)
