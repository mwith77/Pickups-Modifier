# Inventory and Pickups Modifier

Changes what the player picks up in The Last of Us Part I and how much of each
item they may carry, from an in-game menu, at any time during play. It multiplies
every pickup of the crafting materials, the kinds of ammunition, the items, and
the parts and supplements, and sets a carry limit for each. A change in the menu
reaches the pickups already in the level and the limits on what the player is
already holding, with no reload.

Each type is divided into up to ten ranges by how much a pickup holds, and each
range has its own multiplier, so a small pickup and a large one can be multiplied
differently.

Version 1.0.1, for `tlou-i.exe` 1.1.5.0. It loads through the
[TLOU Plugin Loader](https://github.com/mwith77/TLOU-Plugin-Loader), and its
settings appear in the game's menus through the
[Mod Menu Framework](https://github.com/mwith77/Mod-Menu-Framework).

## Files

| File | Contents |
| --- | --- |
| `inventory_and_pickups_modifier.cpp` | The modifier: the multipliers, carry limits and the hooks that apply them. |
| `tlou_plugin_sdk.h` | The loader header the modifier includes. |
| `inventory_and_pickups_modifier.ini` | The multipliers, ranges and carry limits, with their defaults. |
| `inventory_and_pickups_modifier.menupage` | The settings page the framework draws. |
| `build_inventory_and_pickups_modifier.bat` | Builds the DLL. |

## Building

From the x64 Native Tools Command Prompt for Visual Studio, which provides
`cl`, run `build_inventory_and_pickups_modifier.bat` in this folder. It builds
`inventory_and_pickups_modifier.dll`.

## Installing

Laid out as it goes into the game folder:

    Mods\Inventory and Pickups Modifier\    the DLL and its ini
    Mods\Mod Menu Framework\Mod Menus\      the modifier's menupage

It needs the TLOU Plugin Loader, which loads it. With the Mod Menu Framework its
settings show in the game's menus; without the framework the same settings are
edited in the ini directly.

Uninstalling is deleting `Mods\Inventory and Pickups Modifier` and the menupage.
No save depends on the modifier: without it, pickups and carry limits are the
game's own again.

## Logs

The modifier keeps its log in `Mods\Inventory and Pickups Modifier\Logs` and
writes only failures and errors. Each log holds one run: at start, the previous
run's log is moved to `Logs\Archive`.
