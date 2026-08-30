# anvil_parity

Offline check of the Anvil container (`AnvilRegion`) and the NBT writer
(`Game::Nbt::Writer`) against region files Minecraft actually wrote.

No engine, no window, no world load — five translation units and a save
folder. Modelled on `tools/blockstate_parity` for the same reason that exists:
this is bit-level code whose failure modes are silent.

## Build & run

```bash
clang++ -std=c++20 -I src -I ext/zlib -g -O1 \
    tools/anvil_parity/anvil_parity.cpp \
    src/server/world/storage/anvil/AnvilRegion.cpp \
    src/server/world/storage/anvil/PaletteCodec.cpp \
    src/server/world/storage/NBTParser.cpp \
    src/common/nbt/NbtWrite.cpp \
    src/common/core/Log.cpp \
    -lz -o /tmp/anvilparity

/tmp/anvilparity "$HOME/Library/Application Support/minecraft/saves/New World (1)"
```

With no argument it picks the world with the most region files under the
user's Minecraft install. It **never writes** into the world it is given —
every write test happens in a scratch directory under `$TMPDIR`.

## What it checks

| Check | Why |
|---|---|
| Writer specifics | Empty list declares element type `TAG_End` (matches `ListTag.identifyRawElementType`); non-empty keeps its type; modified-UTF8 round-trips an emoji as a 6-byte surrogate pair and `U+0000` as `C0 80`; identical calls produce identical bytes; bit 63 of a packed long survives; a refusal latches instead of emitting a half-tag |
| Gate A — semantic round-trip | Every chunk vanilla wrote is parsed, re-emitted through our writer, re-parsed, and the tag trees compared. Not a byte comparison: `NBTTagCompound` is an `unordered_map`, so key order is not recoverable, and matching Mojang's deflate output is not a property worth depending on |
| Gate B — >32 MB region | Writes past 8192 sectors, which is where a `std::bitset<8192>` sector bitmap would have thrown, then re-reads every chunk |
| External `.mcc` | A payload ≥ 256 sectors moves to `c.<cx>.<cz>.mcc` (absolute chunk coords, same folder) leaving a 5-byte stub, and reads back intact |
| Rewrite churn | 144 rewrites over 12 slots at varying sizes, re-reading **every live chunk after every write**. This is what catches a free-before-allocate ordering: the allocator hands back sectors that still hold the only copy |
| Gate H — nested writer shapes | The structures the entity codec added, emitted and re-parsed: a list of compounds opened while already inside a list compound (`Passengers`), the same two deep (a jockey riding a jockey), a compound opened inside a list compound and recursing (`hidden_effect`), and `attributes` beside a scalar. Keys written **after** each nested list are checked to land at the right depth — a depth mistake here does not throw, it silently reparents every following key |
| Gate C — corruption | 400 mutated copies (truncations and byte-flips) read end to end: no hang, no crash, no unbounded allocation |

## Measured

~50,600 chunks / ~1.5 GB of real vanilla NBT across four Minecraft versions
(1.12.2, 1.16.5, 1.21.7, 1.21.8), zero semantic differences.

## Note on sanitizers

`-fsanitize=address` currently hangs in dyld initialisation on this macOS
before `main` runs — an ASan/dyld issue, unrelated to this code. The harness
is written to be ASan-clean; re-try it after a toolchain update.
