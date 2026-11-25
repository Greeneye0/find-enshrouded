# Find – Base Storage Search

An Enshrouded mod: press **Shift+F** in your base, type an item, a kind of item or a chest
name, and the chests that hold it light up. Open one and the items you searched for glow gold.

![Find: the matching chests light up](dist/nexus/1-hero.png)

## Install

1. Close Enshrouded.
2. Download the release zip and copy `wsock32.dll` into the game folder, next to
   `enshrouded.exe` (Steam: right-click Enshrouded > Manage > Browse local files).
3. Start the game, stand inside one of your bases and press Shift+F.

Linux (Steam Deck, Proton, Wine): the release zip's `greeneye/linux/LINUX.txt` has the steps. In short, rename `dbgcore.dll` to `dinput8.dll` and add `WINEDLLOVERRIDES="dinput8=n,b" %command%` to the game's Steam launch options (needs Greeneye Loader 1.2.4, included).

Uninstall: delete `wsock32.dll`, plus `find.ini` and `find.log` if present. Client-side only:
servers and other players need nothing. Find only reads your chests; it never moves an item.

## Use

1. Stand inside one of your bases and press **Shift+F** (changeable in `find.ini`). Find
   only opens while you are playing or in your backpack, never while you type in chat,
   rename something or write on a signpost. Outside a base it only says so: the field
   stays empty and Enter or Esc closes it.
2. Type an item, a chest name, or a kind of item. Suggestions appear as you type.
   - **Tab / Shift+Tab** (or Down / Up) go through the suggestions, or click one with the
     mouse to find it right away. **Enter** finds, **Esc** cancels.
   - **Exact match** (**Ctrl+E**, or Y on a controller) is a checkbox in the search box. Off,
     a search of several words also lists names with only some of the words, lower down
     (`ring mana` lists the Mana rings first, then other rings); on, only names that hold
     the whole text. The message line says how many results it hides. A bar
     beside the list shows where you are when there are more suggestions than fit.
   - A suggestion picked with Tab finds just that item, chest or kind.
   - Typed text finds every name with a word starting with it: `stone` finds Stone and
     Rough Stone Block, `arrow` finds every arrow but not Narrow Wall.
3. The matching chests pulse, and gold stars on the game's compass show which way they are
   (with [The Worst Map Mod](https://www.nexusmods.com/enshrouded/mods/220) 1.3.54 or newer
   they show on the World Map and the minimap too: Find exports `FindFoundTargets` for it).
   Open one and the found items in it pulse gold for about five
   seconds, the first time you open it. The same in a crafting menu: open a lit station or NPC
   and the found items glow in its recipe list. A chest's pulse ends a few seconds after you reach it
   or open it; every pulse ends after three minutes, with your next search, or when you leave
   the base.

## The skill tree

On the Skills page, **Shift+F** (LB+X on a controller) searches the skill tree instead of your
chests. Type part of a skill's name or of what it does, in the game's language; categories such
as `Guardian` are listed too. The skills found pulse gold for 5 seconds, and a single match
brings the view to it. Esc closes only the search. It works however you open the Skills page (H, or the Skills tab of any menu page),
and on the Reskill page.

## Settings

`find.ini` (written with the defaults on the first start; settings an older `find.ini` lacks
are added to it) holds:

| Setting | Default | What it does |
| --- | --- | --- |
| `hotkey` | `Shift+F, Pad:LB+X` | what opens the search: a key, with `Shift+`, `Ctrl+` and/or `Alt+` in front if you like, and a controller button after a comma (hold LB, press X) |
| `confirm_key`, `close_key` | `Enter, Pad:A`, `Esc, Pad:B` | find, close |
| `next_key`, `previous_key`, `up_key`, `down_key` | `Tab`, `Shift+Tab`, `Up, Pad:Up`, `Down, Pad:Down` | move through the list; with a controller and nothing typed, Up and Down list everything in the base |
| `glow_seconds` | `180` | the found chests glow at most this long (10-3600), or until your next search |
| `near_meters` | `2.5` | coming this close to a found chest counts as reaching it (0.5-20) |
| `near_seconds` | `3` | a chest stops glowing this long after you reach or open it (0-60) |
| `exact_match`, `exact_key` | `0`, `Ctrl+E, Pad:Y` | the Exact match checkbox (saved when you switch it in the search box) and what switches it |
| `through_walls` | `glow` | how lit chests, NPCs and stations show through walls: `glow` (a glowing 3D copy), `icon` (a marker over each, lighter on the graphics card) or `silhouette` (an outline on the HUD) |
| `debug_deposit_glow` | not in the file | debug: add `debug_deposit_glow = 1` and Find lights things the way Quick Deposit lights its chests, in gold: no 3D copies, an outline on the HUD through walls |
| `debug_fps_test` | not in the file | debug: add `debug_fps_test = 1` and after each search Find keeps the glow on for about two minutes, switches its parts off one at a time and writes each one's frame rate to `find.log` |

Restart the game after changing them.

## What you can search for

- **Items**, by the name the game shows.
- **Kinds of items**: categories (Consumables, Ammunition, Materials, ...), rarities
  (Uncommon to Legendary), item types (One-Handed Axe, ...) and perks (Backstab Damage,
  Multishot, ...).
- **Who can make it**: the NPCs and crafting stations of the base that can make a found item
  light up too (`bandage` lights the Hunter). Items no chest holds but the base can make are
  suggested as `can be made here`, or `made here, not unlocked yet` when you haven't unlocked
  the recipe.
- **Item levels**: `level 55` finds items that are level 55 now or will be once fully
  upgraded (Cutthroat Boots at 10/15 count for both 10 and 15).
- **Effects**: what a food or potion gives, such as `oxygen` for the foods with Maximum Oxygen,
  or `stamina`. Dishes no chest holds light up the station or NPC that cooks them.
- **Chest names you gave yourself** with Rename. The game's own chest titles,
  "Storage Chest" and its variants (Large Storage Chest and the like), are not searched:
  every unnamed chest would match.

Suggestions say what they are: `Granite Block — 996 in 1 chest`, `Perk: Backstab Damage — 12
items in 3 chests`, `Chest: Arrows`. Find follows the game's language: item, perk, category
and rarity names are the game's own, and Find's few texts of its own come in English, German,
French, Spanish, Italian, Portuguese, Polish, Russian, Ukrainian, Japanese, Korean, Chinese and
Turkish.

![The search box](dist/nexus/2-search.png)

## Building

Needs Windows, the Visual Studio 2022 Build Tools (C++ x64) and nothing else.

- `build.bat` builds the release: `bin\wsock32.dll`, a proxy DLL that forwards every Winsock
  call to the Windows `wsock32.dll` and loads Find inside the game.
- `tools\tests.bat` and the other `tools\*-tests.bat` run the offline tests (hook
  trampolines, hotkey parsing, the proxy, the hot loader, the renderer helpers).
- `tools\make_images.py` builds the Nexus page images from screenshots (needs Pillow).

### Development build

`build-dev.bat` builds `bin-dev\loader\find.dll` with diagnostic logging (`FIND_DEV`). Under
the Greeneye Loader with `greeneye\hotreload.enable`, `..\tools\hot-reload.ps1 find -Dev`
swaps it into the running game without a restart (see [HOTLOADING.md](HOTLOADING.md)). New hook
sites still need a restart. Use the release build for playing and for releases.

## How it works

Find is a proxy DLL loaded by the game. It finds the game functions it needs by byte
signatures (so it switches itself off rather than crash after a game update), and hooks the
in-game UI to draw its search box with the game's own text-input dialog, its chest highlight
through the game's renderer, and the gold boxes in an opened chest through the game's
inventory slot drawing. Base, chest and item data are read from the client's own state, the
recipes (which NPC or station makes an item) from the game's loaded recipe registry; names
come from the game's localisation, so the mod follows the game's language.

## License

MIT, see [LICENSE](LICENSE). Not affiliated with Keen Games. Made with the help of AI (Claude
by Anthropic, with parts by OpenAI Codex).
