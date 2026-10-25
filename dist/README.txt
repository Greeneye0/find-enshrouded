FIND - BASE STORAGE SEARCH  -  Enshrouded mod, version 1.1.8
=============================================================

Press Shift+F in your base, type what you are looking for, and the chests that hold it
light up. Open one and the items you searched for glow gold inside it.


INSTALL
-------
1. Close Enshrouded.
2. In Steam: right-click Enshrouded > Manage > Browse local files.
   This opens the game folder (the one with enshrouded.exe), usually
   C:\Program Files (x86)\Steam\steamapps\common\Enshrouded
3. Copy everything from this zip into that folder, keeping the folders:
       dbgcore.dll                 next to enshrouded.exe
       greeneye\find\find.dll
       greeneye\find\mod.json
4. Start the game, stand inside one of your bases and press Shift+F.

Updating from Find 1.0.x: delete wsock32.dll from the game folder. Your find.ini moves to
greeneye\find\ by itself on the first start.

Install it by hand: Vortex does not know the Greeneye Loader yet.


USE
---
- Type an item, a chest name, or a kind of item. Suggestions appear as you type, with how
  many there are and in how many chests:  Granite Block - 996 in 1 chest
- Tab / Shift+Tab (or Down / Up) go through the suggestions, or click one with the mouse.
  The mouse wheel scrolls a long list. Enter (or clicking the chosen suggestion again)
  finds; Esc cancels.
- Typed text finds every name with a word starting with it: "stone" finds Stone and Rough
  Stone Block, "arrow" finds every arrow but not Narrow Wall.
- A suggestion picked with Tab or the mouse finds just that item, chest or kind.
- The matching chests pulse with the game's own highlight. A chest stops pulsing a few
  seconds after you reach it or open it. Every pulse ends after three minutes, with your
  next search, or when you leave the base.
- Open a matching chest and the found items in it glow gold for about five seconds, the
  first time you open it.

Find opens only while you play or have your backpack open, never while you type in chat,
rename something or write on a signpost.


WHAT YOU CAN SEARCH FOR
-----------------------
- Items, by the name the game shows.
- Kinds of items: categories (Consumables, Ammunition, Materials, ...), rarities (Uncommon
  to Legendary), item types (One-Handed Axe, ...) and perks (Backstab Damage, ...). They
  are labelled in the list:  Perk: Backstab Damage - 12 items in 3 chests
- Chest names you gave yourself with Rename. The game's own chest titles ("Storage Chest"
  and its variants) are not searched, as every unnamed chest would match.

Find searches the chests of the base you stand in. Outside a base it says so.


SETTINGS
--------
greeneye\find\find.ini (written with the defaults on the first start; settings an older
find.ini lacks are added to it):
    hotkey = Shift+F     the key that opens the search. A letter, a digit or F1-F12,
                         optionally with Shift+, Ctrl+ and/or Alt+ in front.
                         Examples: Ctrl+F, Alt+G, F6.
    glow_seconds = 180   the found chests glow at most this long (10-3600), or until
                         your next search
    near_meters = 2.5    coming this close to a found chest counts as reaching it (0.5-20)
    near_seconds = 3     a chest stops glowing this long after you reach or open it (0-60)
Restart the game after changing them.


LANGUAGES
---------
Find follows the language set in the game. Item, perk, category and rarity names are the
game's own. Find's few texts of its own are in English, German, French, Spanish, Italian,
Portuguese, Polish, Russian, Ukrainian, Japanese, Korean, Chinese and Turkish.


UNINSTALL
---------
Delete greeneye\find. If no other mod is left in the greeneye folder, delete dbgcore.dll
and the greeneye folder too.


GOOD TO KNOW
------------
- Client-side only. Servers need nothing installed, and other players do not need the mod.
- Only reads your chests: it never moves, adds or removes items.
- dbgcore.dll is the Greeneye Loader: it loads the mods in the greeneye folder (Find,
  Continue, Auto Loot and Quick Deposit share it) and forwards everything else to Windows'
  own dbgcore.dll.
- Works next to other mods. Where another mod (Creative Mode, Quick Deposit, Auto Loot)
  hooks the same game function, Find chains onto its hook instead of replacing it.
- Made for the current game version (build 1273043). After a game update the mod checks
  that it still recognises the game; if not, it switches itself off and writes "inactive"
  to find.log instead of risking a crash.
- If something goes wrong, greeneye\find\find.log says what the mod did and
  greeneye\loader.log what the loader did; please post both with a bug report.


CHANGES
-------
1.1.8
- Found chests light up again when another mod outlines things too (The Worst Map Mod's
  GG chest glow): Find no longer depends on being first in line for the game's highlight.

1.1.7
- Switching to another world without restarting the game no longer makes Find say
  "This base has no storage chests to search" in a base full of chests (a 1.1.6 bug).
Reported bugs fixed:
- No storage chests found after switching worlds.

1.1.6
- Planted bushes are no longer treated as chests. The flame altar counts their fruit toward
  crafting, so searching "hazelnut" lit up every Hazelnut bush in the base, and with many
  of them in view the game crashed.
- Crafting stations (spinning wheel, kiln, tanning station and the like) are no longer
  listed as chests in a base that has planted bushes.
- Through-wall chest outlines have a detail limit, so many outlines at once can no longer
  crash the game.
Reported bugs fixed:
- Game crash right after searching "hazelnut" (reported by Lomack).

1.1.5
- Works in a base whose altar lists no chests (a small flame altar): Find searches the
  storage chests inside the base instead of saying it has none.
Reported bugs fixed:
- "This base has no storage chests to search" with a chest in the base (reported by
  Andydark287).

1.1.4
- An NPC's Summoning Staff is found as "Summoning Staff" too: the game names it after the
  NPC ("Fenrig Mills, Jr."), so only the NPC's name found it. Find lists it as
  "Summoning Staff (Fenrig Mills, Jr.)".
Reported bugs fixed:
- Searching "summon" did not find Summoning Staffs (reported by HadToRegister).

1.1.3
- Fixed: in some bases a search missed items past a chest's first row and every chest not
  linked to the flame altar ("purple" found nothing although Purple Berries were in a
  chest). It happened when the base's first chest had only one row.
Reported bugs fixed:
- Search did not find Purple Berries stored in the base.

1.1.2
- Works alongside the XHL mods (Vein Mining, Area Harvest, Free Climbing and the others):
  with Find installed, their F10 settings menu was missing and Area Harvest did not start.
  Find now hooks only its own calls to the game functions they look up, never the
  functions themselves.
Reported bugs fixed:
- XHL mods (Vein Mining, Auto Harvest) stopped working and their F10 menu was missing with
  Find installed (reported by HadToRegister).

1.1.1
- Fixed: in a base where your game had not loaded the base's first chest yet (a large base,
  often on a server), every search found nothing. Find now tries every chest of the base,
  then your own backpack.
- Fixed: after switching worlds without restarting the game (your own world, then a server),
  Find searched the old world and found nothing.

1.1.0
- Runs through the Greeneye Loader (dbgcore.dll), shared with Quick Deposit, Continue and
  Auto Loot. find.ini and find.log now live in greeneye\find\.
- New settings in find.ini: glow_seconds (how long chests glow at most), near_meters (how
  close counts as reaching a chest) and near_seconds (how long a chest keeps glowing after
  you reach or open it). Raise near_seconds to keep chests lit while you walk between them.
- Found chests glow at most three minutes by default (was five).
- The found items inside a chest glow only the first time you open it.
Reported bugs fixed and requests:
- Items glowed again every time a chest was reopened (reported by Dacrenzio).
- Chests should stay highlighted longer (requested by HadToRegister).

1.0.2
- Works with Creative Mode. Find 1.0.1 switched itself off next to it ("signature not
  found: LocaText" in find.log); it now finds the game functions Creative Mode hooks and
  chains onto its hooks.
- Finds a game function another mod has already hooked by the code behind that mod's
  hook, so load order between mods matters less.
- Inside a base without storage chests, Find says "This base has no storage chests to
  search." instead of claiming you are not in a base.
- Outside a base the search field is switched off; Enter or Esc closes it.
Reported bugs fixed:
- Find inactive next to Creative Mode (reported by Dacrenzio, cnrat and Bigdaddy2I42).
- "Not in a base" shown in a base that has no storage chests (reported by GreeneyeDS).

1.0.1
- Esc hands the keyboard back to the game.

1.0.0
- First release.
