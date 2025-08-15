# Adding a New Block to MyVoxelGame

This guide documents all the places that need to be modified when adding a new block to the game.

## Required Steps

### 1. Add Block ID to Enum
**File:** `src/common/world/block/Blocks.hpp`

Add a new entry to the `BlockID` enum before the `Count` entry:
```cpp
enum class BlockID : uint16_t {
    // ... existing blocks ...
    YourNewBlock = [next_number],
    
    Count // Always keep this as the last entry
};
```

### 2. Register Block in BlockRegistry
**File:** `src/common/world/block/BlockRegistry.cpp`

Add a registration call in the `Initialize()` function:
```cpp
RegisterModelBlock(BlockID::YourNewBlock, "Your Block Name", true, "your_block_model");
```

Parameters:
- `BlockID`: The enum value from step 1
- `Display Name`: Human-readable name shown in game
- `Is Solid`: Whether the block has collision (true/false)
- `Model Name`: Name of the JSON model file (without .json extension)

### 3. Add Minecraft Name Mapping (for world loading)
**File:** `src/server/world/storage/SectionDataUnpacker.cpp`

Add a mapping in the `InitializeBlockMappings()` function:
```cpp
s_nameToBlockId["minecraft:your_block_name"] = BlockID::YourNewBlock;
```

This allows the game to recognize the block when loading Minecraft world files.

## Required Assets

### 4. Block Model File
**Location:** `assets/models/block/your_block_model.json`

Create a JSON model file that defines the block's geometry and texture mappings.

### 5. Block Texture
**Location:** `assets/textures/block/`

Add the texture image file(s) referenced by your model. The texture atlas builder will automatically include them.

## Example: Adding Deepslate Redstone Ore

1. **Blocks.hpp:**
```cpp
DeepslateRedstoneOre = 69,
```

2. **BlockRegistry.cpp:**
```cpp
RegisterModelBlock(BlockID::DeepslateRedstoneOre, "Deepslate Redstone Ore", true, "deepslate_redstone_ore");
```

3. **SectionDataUnpacker.cpp:**
```cpp
s_nameToBlockId["minecraft:deepslate_redstone_ore"] = BlockID::DeepslateRedstoneOre;
```

4. **Model:** `assets/models/block/deepslate_redstone_ore.json`
5. **Texture:** `assets/textures/block/deepslate_redstone_ore.png`

## Notes

- Block IDs must be unique and sequential
- The `Count` entry in the enum must always remain last
- Model names should match Minecraft conventions (lowercase, underscores for spaces)
- For transparent blocks like glass or leaves, additional rendering setup may be required
- Blocks with special behaviors (like water, lava, or interactive blocks) require additional code beyond these basic steps