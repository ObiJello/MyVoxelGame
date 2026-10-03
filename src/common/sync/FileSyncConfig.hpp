// File: src/common/sync/FileSyncConfig.hpp
//
// One-file configuration for the optional chat.db migration feature.
//
// This whole feature exists to move ONE of the user's own large files (their
// iMessage `chat.db`, ~400 MB) from their old Mac to their new one, using an
// rsync-style binary delta so only changed bytes are sent. It is the user's
// own data on the user's own machines.
//
// The two Macs connect the SAME WAY the game connects friends: through the
// ObeyCraft friends service (the user is friends with themselves, the source
// account being e.g. "obeyswife"). The source makes itself reachable with the
// same UPnP-direct + relay-fallback mechanism hosting uses; the client reaches
// it directly when possible, otherwise relayed through the friends service.
// The transport is fully isolated from the game's own friends connection — it
// opens its OWN dedicated service connection and never touches the game's
// presence, relay handler, or FriendsClient.
//
// It is OFF by default and must stay a literal no-op until the user sets a
// single value below. When `kFileSyncSourceUsername` is empty (the committed
// default) nothing in this module ever runs, allocates, or starts a thread —
// the game and launcher behave exactly as if the feature did not exist.
//
// THE ONLY THING THE USER CONFIGURES is `kFileSyncSourceUsername`: the game
// player name of the SOURCE Mac (the old Mac that still has the real chat.db).
// Role detection is automatic from that name; there are no roles to set, no
// run ids, and no addresses to configure. Build once, run on both Macs (both
// signed into the friends service from the launcher); each self-selects.
//
// See docs/file-sync.md for the full workflow and safety guarantees.
#pragma once

#include <cstdint>

namespace Sync {

    // ── The one switch ──────────────────────────────────────────────────────
    // Empty string = feature fully OFF (committed default). Set this to the
    // game player name used on the OLD Mac (the one that holds the live
    // chat.db), e.g. "obeyswife". Compared case-insensitively against this
    // machine's own local player name:
    //   • local name == this value  → this Mac is the SOURCE (it serves).
    //   • local name != this value  → this Mac is a CLIENT (it pulls once).
    //
    // IMPORTANT: the NEW Mac must sign into the friends service under a
    // DIFFERENT account/player name (or as a guest) than this value, or it
    // would think it is the source.
    inline constexpr const char* kFileSyncSourceUsername = "obeyswife";

    // TCP port the source maps via UPnP and serves the delta on for the
    // DIRECT path. The relayed path rides the friends service's own port.
    // Deliberately not 25565 (game) or 25570 (friends service).
    inline constexpr uint16_t kFileSyncPort = 25580;

    // Source: the live iMessage database to serve (read-only, opaque bytes).
    inline constexpr const char* kServeFilePath = "~/Library/Messages/chat.db";

    // Client: the file to create/update on the new Mac. NEVER written in
    // place — reconstructed to "<target>.incoming", verified, then atomically
    // swapped in with a timestamped backup of any existing target kept beside
    // it. (Tilde-expanded at runtime.)
    inline constexpr const char* kTargetFilePath = "~/Documents/Messages/chat.db";

    // Dry run: do everything (discover, negotiate the delta, reconstruct and
    // verify the result) EXCEPT the final backup+swap. Logs the delta size,
    // the percentage of bytes reused, and the verification result. The real
    // target file is never touched.
    inline constexpr bool kFileSyncDryRun = false;

} // namespace Sync
