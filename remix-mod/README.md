# Hard Truck 2 RTX Remix mod assets

Copy `HT2_RTX/` to `<game>/rtx-remix/mods/HT2_RTX/`. Generated textures are not committed, so build them first:

```
python tools/gen_rain_normal.py HT2_RTX/textures/rain_glass_normal.dds preview.png
```

- `mat_9CB3B053B225F1AC`: windshield rain layer → wet thin glass (translucent, animated raindrop normal map, 4x2 sprite sheet).
  The texture must not be in `rtx.ignoreTextures`, and `rtx.recomputeTextureHashOnWrite` must stay off (the game rewrites this texture every frame).
