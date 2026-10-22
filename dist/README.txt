FIND - BASE STORAGE SEARCH  -  Enshrouded mod, version 1.0.2
=============================================================

Press Shift+F in your base, type what you are looking for, and the chests that hold it
light up. Open one and the items you searched for glow gold inside it.


INSTALL
-------
1. Close Enshrouded.
2. Copy wsock32.dll into the game folder, next to enshrouded.exe.
   (Steam: right-click Enshrouded > Manage > Browse local files.)
3. Start the game, stand inside one of your bases and press Shift+F.


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
  seconds after you reach it or open it, and every pulse ends when you leave the base.
- Open a matching chest and the found items in it glow gold for about five seconds.

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


HOTKEY
------
find.ini in the game folder (written on the first start) sets the key:
    hotkey = Shift+F
A letter, a digit or F1-F12, optionally with Shift+, Ctrl+ and/or Alt+ in front.
Examples: Ctrl+F, Alt+G, F6. Restart the game after changing it.


LANGUAGES
---------
Find follows the language set in the game. Item, perk, category and rarity names are the
game's own. Find's few texts of its own are in English, German, French, Spanish, Italian,
Portuguese, Polish, Russian, Ukrainian, Japanese, Korean, Chinese and Turkish.


UNINSTALL
---------
Delete wsock32.dll from the game folder, plus find.ini and find.log if present.


GOOD TO KNOW
------------
- Client-side only. Servers need nothing installed, and other players do not need the mod.
- Only reads your chests: it never moves, adds or removes items.
- Works next to other mods: it has its own DLL name and forwards every Winsock call to the
  real Windows wsock32.dll. Where another mod (Creative Mode, Quick Deposit, Auto Loot)
  hooks the same game function, Find chains onto its hook instead of replacing it.
- Made for the current game version (build 1273043). After a game update the mod checks
  that it still recognises the game; if not, it switches itself off and writes "inactive"
  to find.log instead of risking a crash.
- find.log in the game folder says what the mod did.


CHANGES
-------
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
