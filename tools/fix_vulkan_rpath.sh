#!/bin/bash
# Fix the executable's Vulkan library reference to use @rpath (bundled copy).
# Usage: fix_vulkan_rpath.sh <path_to_executable>

EXECUTABLE="$1"
if [ -z "$EXECUTABLE" ]; then
    echo "Usage: $0 <executable>"
    exit 1
fi

# Find the current vulkan library reference in the executable
VULKAN_REF=$(otool -L "$EXECUTABLE" | grep libvulkan | head -1 | awk '{print $1}')

if [ -z "$VULKAN_REF" ]; then
    echo "No libvulkan reference found in $EXECUTABLE"
    exit 0
fi

if [ "$VULKAN_REF" = "@rpath/libvulkan.1.dylib" ]; then
    echo "Vulkan reference already uses @rpath"
    exit 0
fi

echo "Changing $VULKAN_REF -> @rpath/libvulkan.1.dylib"
install_name_tool -change "$VULKAN_REF" "@rpath/libvulkan.1.dylib" "$EXECUTABLE"

# Re-sign after modification (install_name_tool invalidates the code signature)
codesign --force --sign - "$EXECUTABLE" 2>/dev/null || true
