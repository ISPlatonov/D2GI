# Hard Truck 2 RTX Remix mod assets

Copy `HT2_RTX/` to `<game>/rtx-remix/mods/HT2_RTX/`. Generated textures are not committed, so build them first:

```
python tools/gen_rain_normal.py HT2_RTX/textures/rain_glass_normal.dds preview.png
```

- `mat_1AB38C53610F8621`: windshield rain layer → wet thin glass (translucent, animated raindrop normal map, 4x2 sprite sheet).
  This is the stable Remix hash of that texture after `StaticBlitTextures` fills it with constant content. The texture must not be in `rtx.ignoreTextures`. The earlier hash `9CB3B053B225F1AC` was from before that fill and does not match the current DLL.
