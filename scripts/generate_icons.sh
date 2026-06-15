#!/usr/bin/env bash
# generate_icons.sh
# Converts resources/icons/appicon.svg into all platform icon formats.
#
# Required tools (at least one rasterizer):
#   inkscape     - best quality SVG rendering (recommended)
#   rsvg-convert - fallback (librsvg)
#   cairosvg     - fallback (Python: pip install cairosvg)
#
# For .ico (Windows):
#   convert      - ImageMagick
#   magick       - ImageMagick v7
#   Python PIL   - fallback (pip install Pillow)
#
# For .icns (macOS):
#   iconutil     - built-in on macOS (required)
#   png2icns     - Linux alternative (libicns package)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
SVG="$REPO_ROOT/resources/icons/appicon.svg"
OUT="$REPO_ROOT/resources/icons/generated"

if [[ ! -f "$SVG" ]]; then
    echo "ERROR: $SVG not found."
    echo "Put your logo at: resources/icons/appicon.svg"
    echo "Tip: use a transparent background so macOS 26 Liquid Glass works automatically."
    exit 1
fi

mkdir -p "$OUT"

# ── Detect rasterizer ────────────────────────────────────────────────────────
rasterize() {
    local size=$1 out=$2
    if command -v inkscape &>/dev/null; then
        inkscape --export-type=png --export-filename="$out" \
                 --export-width="$size" --export-height="$size" \
                 "$SVG" &>/dev/null
    elif command -v rsvg-convert &>/dev/null; then
        rsvg-convert -w "$size" -h "$size" -o "$out" "$SVG"
    elif command -v cairosvg &>/dev/null; then
        cairosvg "$SVG" -o "$out" -W "$size" -H "$size"
    elif python3 -c "import cairosvg" &>/dev/null 2>&1; then
        python3 -c "
import cairosvg, sys
cairosvg.svg2png(url=sys.argv[1], write_to=sys.argv[2],
                 output_width=int(sys.argv[3]), output_height=int(sys.argv[3]))
" "$SVG" "$out" "$size"
    else
        echo "ERROR: No SVG rasterizer found. Install inkscape, rsvg-convert, or cairosvg."
        exit 1
    fi
}

# ── Generate PNG sizes ───────────────────────────────────────────────────────
echo "Generating PNG sizes..."
for SIZE in 16 32 48 64 128 256 512 1024; do
    rasterize "$SIZE" "$OUT/${SIZE}.png"
    echo "  ${SIZE}x${SIZE} -> generated/${SIZE}.png"
done

# ── Windows .ico ─────────────────────────────────────────────────────────────
echo ""
echo "Generating Windows .ico..."
ICO_SIZES=("$OUT/16.png" "$OUT/32.png" "$OUT/48.png" "$OUT/64.png" "$OUT/128.png" "$OUT/256.png")
if command -v magick &>/dev/null; then
    magick "${ICO_SIZES[@]}" "$OUT/appicon.ico"
    echo "  -> generated/appicon.ico (ImageMagick v7)"
elif command -v convert &>/dev/null; then
    convert "${ICO_SIZES[@]}" "$OUT/appicon.ico"
    echo "  -> generated/appicon.ico (ImageMagick)"
elif python3 -c "from PIL import Image" &>/dev/null 2>&1; then
    python3 - "$OUT" <<'PYEOF'
import sys
from PIL import Image
out = sys.argv[1]
imgs = []
for s in [16, 32, 48, 64, 128, 256]:
    imgs.append(Image.open(f"{out}/{s}.png").convert("RGBA"))
imgs[0].save(f"{out}/appicon.ico", format="ICO",
             sizes=[(16,16),(32,32),(48,48),(64,64),(128,128),(256,256)],
             append_images=imgs[1:])
print("  -> generated/appicon.ico (Pillow)")
PYEOF
else
    echo "  SKIP: .ico — install ImageMagick (convert/magick) or Pillow (pip install Pillow)"
fi

# ── macOS .icns ───────────────────────────────────────────────────────────────
echo ""
echo "Generating macOS .icns..."
ICONSET="$OUT/AppIcon.iconset"
mkdir -p "$ICONSET"

declare -A ICNS_MAP=(
    [icon_16x16.png]=16
    [icon_16x16@2x.png]=32
    [icon_32x32.png]=32
    [icon_32x32@2x.png]=64
    [icon_128x128.png]=128
    [icon_128x128@2x.png]=256
    [icon_256x256.png]=256
    [icon_256x256@2x.png]=512
    [icon_512x512.png]=512
    [icon_512x512@2x.png]=1024
)

for NAME in "${!ICNS_MAP[@]}"; do
    SIZE="${ICNS_MAP[$NAME]}"
    cp "$OUT/${SIZE}.png" "$ICONSET/$NAME"
done

if command -v iconutil &>/dev/null; then
    iconutil -c icns -o "$OUT/AppIcon.icns" "$ICONSET"
    echo "  -> generated/AppIcon.icns (iconutil)"
elif command -v png2icns &>/dev/null; then
    png2icns "$OUT/AppIcon.icns" \
        "$OUT/16.png" "$OUT/32.png" "$OUT/64.png" \
        "$OUT/128.png" "$OUT/256.png" "$OUT/512.png"
    echo "  -> generated/AppIcon.icns (png2icns)"
else
    echo "  SKIP: .icns — run this script on macOS (iconutil) or install png2icns"
fi

# Cleanup iconset folder — it was only needed for iconutil
rm -rf "$ICONSET"

echo "Done! Now re-run ./build-fast so the new icons are picked up."
echo "macOS 26 Liquid Glass tip:"
echo "  Your SVG should have a TRANSPARENT background (no background rect)."
echo "  macOS 26 automatically applies the Liquid Glass material to transparent icons."
echo "  For full adaptive support (separate fg/bg layers), use Xcode .xcassets."
