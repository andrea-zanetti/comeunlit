#!/bin/sh
# Copy the 3D models the board uses into lib/3dmodels and point the board
# and the project footprint library at ${KIPRJMOD}/lib/3dmodels/.
# Run in the KiCad project folder, with KiCad closed.
#   sh bundle_models.sh                all models, stock KiCad ones included
#   sh bundle_models.sh --custom-only  leave ${KICAD10_3DMODEL_DIR} models alone
set -eu
CUSTOM_ONLY=0
[ "${1:-}" = "--custom-only" ] && CUSTOM_ONLY=1
STOCK=${KICAD10_3DMODEL_DIR:-/usr/share/kicad/3dmodels}
CUSTOM=${CUSTOM_MODELS:-$HOME/projects/kicad_custom_library/models}
FILES=$(ls *.kicad_pcb lib/*.pretty/*.kicad_mod)
mkdir -p lib/3dmodels
grep -ho '(model "[^"]*"' $FILES | sed 's/^(model "//; s/"$//' | sort -u |
while IFS= read -r m; do
    case "$m" in
        '${KIPRJMOD}'/*) continue ;;
        '${KICAD10_3DMODEL_DIR}'/*)
            [ "$CUSTOM_ONLY" = 1 ] && continue
            src="$STOCK/${m#*\}/}" ;;
        /*) src="$m" ;;
        *) src="$CUSTOM/$m" ;;
    esac
    if [ ! -f "$src" ]; then echo "MISSING: $src" >&2; exit 1; fi
    cp "$src" lib/3dmodels/
    new='${KIPRJMOD}/lib/3dmodels/'$(basename "$m")
    for f in $FILES; do
        M="$m" N="$new" perl -pi -e 's/\(model "\Q$ENV{M}\E"/(model "$ENV{N}"/g' "$f"
    done
    echo "bundled: $(basename "$m")"
done
echo "left outside the project:"
grep -ho '(model "[^"]*"' $FILES | sort -u | grep -v 'KIPRJMOD' || echo "  nothing"
