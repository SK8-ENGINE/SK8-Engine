# Bundled maps

This folder contains the first-party **Blender Feature Park** demonstration:

- `blender_bake_showcase.skate` is ready to load in game;
- `blender_bake_showcase.blend` is its editable, self-contained Blender 5.1
  source with the referenced textures packed into the file.

The map demonstrates authored visuals and collision, material surface channels,
grind rails, a hinged physics door, local lights, baked indirect lighting, and
the day/night world settings supported by the Custom Engine Layer preview.
Experimental NPC routes are disabled.

## Players

The Windows release already places the `.skate` package in its `maps` folder.
On a fresh installation it is selected automatically. Add other `.skate`
packages beside it, then use **Settings > Maps** to refresh, select, and load
them.

## Map creators

Install the add-on from `Blender Map Tools/owned_world_material_addon.zip` in
the Windows release, or from `tools/blender_owned_map` in this source tree.
Open the included `.blend` file to inspect a complete working example.

Only commit or distribute maps whose models, textures, audio, and other assets
you have permission to share.
