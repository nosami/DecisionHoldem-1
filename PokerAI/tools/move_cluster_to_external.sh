#!/bin/bash
# Moves the large, git-ignored pretrained cluster/blueprint data files from
# PokerAI/cluster/ to an external drive, replacing them with symlinks so the
# engine's existing relative paths ("cluster/river_hand_cluster.bin", etc.)
# keep working completely unmodified -- ifstream/fopen transparently follow
# symlinks on both macOS and Linux, so no source changes are required.
#
# Safety: copies first, verifies byte-for-byte size match, and only then
# deletes the original and creates the symlink. Aborts on any mismatch
# without touching the original. Never touches the small, git-tracked
# preflopallin1326.1225.bin or the tiny preflop_hand_cluster.bin (10KB) --
# only files large enough to matter for disk-space relief are moved.
#
# Usage:
#   ./move_cluster_to_external.sh "/Volumes/Seagate Desktop Drive/DecisionHoldem_cluster_data"
set -euo pipefail

if [ $# -ne 1 ]; then
	echo "Usage: $0 <destination_directory_on_external_drive>" >&2
	exit 1
fi

DEST_DIR="$1"
SRC_DIR="$(cd "$(dirname "$0")/../cluster" && pwd)"

mkdir -p "$DEST_DIR"

echo "Source:      $SRC_DIR"
echo "Destination: $DEST_DIR"
echo

# blueprint_stgy.dat and blueprint_strategy.dat are the same file (hardlinked,
# same inode) -- copy the underlying content only once, as "blueprint_stgy.dat"
# on the external drive, then symlink both names back to it.
BIG_FILES=(
	"river_hand_cluster.bin"
	"turn_hand_cluster.bin"
	"flop_hand_cluster.bin"
	"sevencards_strength.bin"
	"blueprint_stgy.dat"
)

for f in "${BIG_FILES[@]}"; do
	src="$SRC_DIR/$f"
	dst="$DEST_DIR/$f"
	if [ ! -e "$src" ]; then
		echo "SKIP: $f not present at $src (already moved?)"
		continue
	fi
	if [ -L "$src" ]; then
		echo "SKIP: $f is already a symlink at $src"
		continue
	fi
	echo "=== Copying $f ($(du -h "$src" | cut -f1)) ==="
	rsync -a --progress "$src" "$dst"

	src_size=$(stat -f%z "$src")
	dst_size=$(stat -f%z "$dst")
	if [ "$src_size" != "$dst_size" ]; then
		echo "ABORT: size mismatch for $f (src=$src_size dst=$dst_size). Original left untouched." >&2
		exit 1
	fi
	echo "Verified size match for $f ($dst_size bytes). Replacing original with symlink."
	rm -f "$src"
	ln -s "$dst" "$src"
done

# blueprint_strategy.dat was hardlinked to blueprint_stgy.dat; if it still
# exists as a real file (not yet converted), remove it and symlink it to the
# same external copy used for blueprint_stgy.dat.
alt="$SRC_DIR/blueprint_strategy.dat"
if [ -e "$alt" ] && [ ! -L "$alt" ]; then
	rm -f "$alt"
	ln -s "$DEST_DIR/blueprint_stgy.dat" "$alt"
	echo "Re-linked blueprint_strategy.dat -> $DEST_DIR/blueprint_stgy.dat"
fi

echo
echo "Done. Final PokerAI/cluster/ listing:"
ls -la "$SRC_DIR"
