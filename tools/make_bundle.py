"""Build HT2-RTX.zip, the install bundle published as a GitHub release.

Layout of the zip:
    mod/      -> <game>/_rtx_remix_mod/  (Workshop local mod folder; WorkshopModSync copies it
                 into the game root on every Steam launch)
    _ht2rtx_tools/ -> <game>/_ht2rtx_tools/  (scripts, and workshop/ = Steam Workshop descriptor + preview)
    *.cmd     -> <game>/  (double-click launchers)
    VERSION.txt

Usage:
    python tools/make_bundle.py --dll path/to/ddraw.dll --out dist/HT2-RTX.zip --version "master 1a2b3c4"
"""
import argparse
import datetime
import os
import shutil
import subprocess
import sys
import tempfile
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PACKAGE = os.path.join(ROOT, "remix-mod", "package")
GEN_RAIN = os.path.join(ROOT, "remix-mod", "tools", "gen_rain_normal.py")
GEN_MATERIALS = os.path.join(ROOT, "remix-mod", "tools", "gen_material_maps.py")
MOD_TEXTURES = os.path.join("rtx-remix", "mods", "HT2_RTX", "textures")
RAIN_DDS = os.path.join(MOD_TEXTURES, "rain_glass_normal.dds")
WIN_TOOLS = os.path.join(ROOT, "tools", "windows")
WORKSHOP = os.path.join(ROOT, "remix-mod", "workshop")


def add_tree(zf, src_dir, arc_prefix):
    for dirpath, _, files in os.walk(src_dir):
        for name in sorted(files):
            full = os.path.join(dirpath, name)
            rel = os.path.relpath(full, src_dir).replace(os.sep, "/")
            zf.write(full, f"{arc_prefix}/{rel}" if arc_prefix else rel)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dll", required=True, help="built ddraw.dll")
    ap.add_argument("--ini", default=os.path.join(ROOT, "D2GI", "d2gi.ini"))
    ap.add_argument("--out", required=True)
    ap.add_argument("--version", default="local")
    args = ap.parse_args()

    with tempfile.TemporaryDirectory() as tmp:
        mod = os.path.join(tmp, "mod")
        shutil.copytree(PACKAGE, mod)
        shutil.copy2(args.dll, os.path.join(mod, "ddraw.dll"))
        shutil.copy2(args.ini, os.path.join(mod, "d2gi.ini"))

        dds = os.path.join(mod, RAIN_DDS)
        os.makedirs(os.path.dirname(dds), exist_ok=True)
        subprocess.run([sys.executable, GEN_RAIN, dds, os.path.join(tmp, "rain_preview.png")], check=True)
        subprocess.run([sys.executable, GEN_MATERIALS, os.path.join(mod, MOD_TEXTURES)], check=True)

        version = f"{args.version}\nbuilt {datetime.datetime.now(datetime.timezone.utc):%Y-%m-%d %H:%M} UTC\n"
        with open(os.path.join(mod, "VERSION.txt"), "w", encoding="utf-8") as f:
            f.write(version)

        os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
        with zipfile.ZipFile(args.out, "w", zipfile.ZIP_DEFLATED) as zf:
            add_tree(zf, mod, "mod")
            add_tree(zf, WORKSHOP, "_ht2rtx_tools/workshop")
            for name in sorted(os.listdir(WIN_TOOLS)):
                full = os.path.join(WIN_TOOLS, name)
                zf.write(full, name if name.endswith(".cmd") else f"_ht2rtx_tools/{name}")
            zf.writestr("VERSION.txt", version)

        print(f"wrote {args.out}")
        with zipfile.ZipFile(args.out) as zf:
            for i in zf.infolist():
                print(f"  {i.file_size:>10}  {i.filename}")


if __name__ == "__main__":
    main()
