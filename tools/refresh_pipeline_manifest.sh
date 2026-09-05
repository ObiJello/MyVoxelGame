#!/bin/bash
# Refresh the shipped pipeline-warm-up seed (assets/vk_pipeline_manifest.txt)
# from this machine's cached manifest. Run after a session that exercised
# everything (all three dimensions, portals a few layers deep, the HUD
# screens), and before a release. See VKBackend::WarmPipelines.
set -e
root="$(cd "$(dirname "$0")/.." && pwd)"
src="$HOME/Library/Application Support/obeycraft/cache/vk_pipeline_manifest.txt"
[ -f "$src" ] || { echo "no cached manifest at $src — play a session on the Vulkan backend first"; exit 1; }
cp "$src" "$root/assets/vk_pipeline_manifest.txt"
echo "seed refreshed: $(head -1 "$root/assets/vk_pipeline_manifest.txt") — $(($(wc -l < "$root/assets/vk_pipeline_manifest.txt") - 1)) pipelines"
