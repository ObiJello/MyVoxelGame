# chat.db migration (file-sync)

A small, self-contained feature for moving **one of your own large files** — your
iMessage database `chat.db` (~400 MB) — from your **old Mac** to your **new Mac**,
using an rsync-style binary delta so only the changed bytes travel. It is your
own data on your own machines.

The two Macs connect the **same way the game connects friends**: through the
ObeyCraft friends service, with UPnP-direct first and a relay fallback. You are
friends with yourself (the source account is e.g. `obeyswife`). The transport is
deliberately **isolated** from the game: it opens its OWN dedicated friends-service
connection and never touches the game's `FriendsClient`, presence, or relay
handler. The whole feature is OFF by default and can never break, slow, or crash
the game or launcher.

Source:
- `src/common/sync/` — `FileSyncConfig.hpp` (config), `FileDeltaSync.{hpp,cpp}`
  (the rsync delta engine + safety, transport-agnostic).
- `src/client/sync/FileSyncService.{hpp,cpp}` — the friends-service transport
  (discovery, UPnP-direct + relay, background thread).
- `tools/friends_server/friends_service.py` — two additive ops (`filesync_offer`,
  `filesync_connect`) that reuse the service's existing relay splice.

---

## The one thing you configure

Open `src/common/sync/FileSyncConfig.hpp` and set **one** value:

```cpp
inline constexpr const char* kFileSyncSourceUsername = "";   // empty = OFF
```

Set it to the **friends/player name used on the OLD Mac** (the Mac that still has
the real `chat.db`), for example `"obeyswife"`. That is the entire configuration.
There are **no roles to pick, no run ids, and no addresses**:

- Empty string (the committed default) → the feature is a literal no-op. No
  thread starts, nothing allocates, nothing connects. The game is byte-for-byte
  unaffected.
- Non-empty → the feature turns on and each Mac decides its own role
  automatically from its local player name (below).

The other constants in that file rarely need changing:

| Constant | Default | Meaning |
|---|---|---|
| `kFileSyncPort` | `25580` | TCP port the source UPnP-maps and serves the DIRECT path on (relay rides the friends service's own port). Not 25565/25570. |
| `kServeFilePath` | `~/Library/Messages/chat.db` | The live DB the source serves. |
| `kTargetFilePath` | `~/Documents/Messages/chat.db` | The file updated on the new Mac. |
| `kFileSyncDryRun` | `false` | Do everything **except** the final swap (see below). |

Paths are tilde-expanded at runtime.

---

## Requirements

- **Both Macs signed into the friends service** (the launcher's normal login —
  `--session` / `--account-id`). A guest (no token) cannot use the service, so
  the feature logs that and does nothing (it retries on a future launch once you
  sign in).
- **The two accounts are friends.** The source account is `kFileSyncSourceUsername`;
  the new Mac signs in under a **different** account/name.
- **The friends service is running the updated `friends_service.py`** (it adds the
  two additive `filesync_*` ops). The change is backward-compatible — it does not
  touch the existing accounts/presence/join/hosting flows — but the service does
  need to be restarted with the new file.

---

## The workflow (what you actually do)

1. Set `kFileSyncSourceUsername` to your old Mac's account name and **build once**.
   Restart the friends service with the updated `friends_service.py`.
2. On the **OLD Mac** (signed in as that account): **quit Messages**, then open the
   game. It becomes the **source**: it UPnP-maps its file-sync port, advertises
   itself to the friends service, and serves `chat.db` (directly and/or relayed).
   It keeps serving quietly through the session — harmless and idempotent.
3. On the **NEW Mac** (signed in as a *different* account that is friends with the
   source): open the game. At the title screen it becomes a **client**, reaches the
   source through the friends service, and updates its `chat.db` once.
   - If it **succeeds**, a marker file (`file_sync_done.txt` in the obeycraft dir)
     is written and it never runs again.
   - If it **fails** (source not online/offering yet, Messages still running on the
     source, network hiccup, verify mismatch), **no marker is written**, so the
     next launch simply tries again. Just relaunch.

### One implication to know

Role detection is by **player name**. The NEW Mac must sign in under a **different**
account/name than `kFileSyncSourceUsername` — otherwise it would also think it is
the source and would never pull. The source is *whichever* Mac runs the game under
that name.

---

## Automatic role detection

At the title screen, if `kFileSyncSourceUsername` is non-empty, the game compares
it (case-insensitively) with this machine's own local player name (the `--name`
value the launcher passes):

- **local name == source username** → this Mac is the **SOURCE**: it serves
  `kServeFilePath`.
- **local name != source username** → this Mac is a **CLIENT**: it reaches the
  source through the friends service and pulls into `kTargetFilePath`.

---

## Connecting over the friends service (UPnP-direct + relay)

This reuses exactly the connectivity the game uses to join a hosted friend, but on
a **dedicated** file-sync connection so the game's own friends session is never
touched.

**Source (old Mac):**
1. Requests a UPnP port mapping on `kFileSyncPort` (`UPnPPortMapper`, the same one
   hosting uses) and runs a small file-sync TCP acceptor on it for the DIRECT path.
2. Opens its own NDJSON connection to the friends service (authenticated with the
   same login token) and sends `filesync_offer` with its UPnP external address.
   It keeps this link alive with periodic pings through the session.
3. When a client asks, the service pushes a one-time **relay ticket**; the source
   dials the service's relay and serves the delta over the spliced tunnel — the
   same splice a relayed game join uses.

**Client (new Mac):**
1. Opens its own NDJSON connection to the friends service and sends
   `filesync_connect` naming the source. The service checks friendship, confirms
   the source is offering, mints a relay ticket, pushes it to the source, and
   returns the ticket plus the source's direct address hint.
2. Tries the **direct** address first (short probe). If reachable, it pulls the
   delta straight from the source's acceptor.
3. Otherwise it **relays**: it dials the service, attaches the ticket, and pulls the
   delta over the spliced tunnel. For two Macs on different networks this is the
   usual path; the bytes pass through your own friends-service machine, which is
   fine — the delta keeps it small, and the full-file fallback still works (just
   larger).
4. If the source is not online/offering within a bounded window (~30 s of polling),
   it logs that and gives up for this run (no marker → retry next launch).

**Access control.** The relay ticket (minted only after a friendship check) is
also the shared secret for the direct path: the client presents it before the
transfer, and the source refuses any connection that does not. So a random
internet scanner that finds the UPnP-mapped port cannot pull the database.

The delta frame codec (length-prefixed + CRC32) runs over whichever stream the
connection yields — a direct socket or the relay tunnel.

---

## The delta algorithm (only changed bytes move)

Classic rsync, receiver-driven. The **client** has the base file (its current
target) and wants the source's authoritative copy:

1. **Client** memory-maps its base file and splits it into ~4 KB blocks. For each
   block it sends the source a weak **rolling checksum** (Adler-style, mod 2¹⁶) and
   a strong **128-bit hash** (MurmurHash3 x64 128). If the client has no base file,
   it sends zero blocks.
2. **Source** memory-maps its current `chat.db` and scans it with the rolling
   checksum. On a weak hit confirmed by the strong hash it emits
   `COPY(baseOffset, len)` (contiguous copies are coalesced); otherwise it
   accumulates `LITERAL` bytes. Literal payloads are compressed with `Core::Deflate`
   (libdeflate, raw deflate) — the project's only deflate path; **no zlib/minizip is
   ever added**.
3. **Source** also sends the new file's total length and a whole-file 128-bit hash.
4. **Client** reconstructs the file **streaming**, to `<target>.incoming`, never
   loading the whole 400 MB into RAM: `COPY` ops are read from the mmap'd base,
   `LITERAL` ops are decompressed, both written out in 1 MiB strides while the
   whole-file hash is computed incrementally.
5. **No base file on the client** → the source's scan finds no matches and streams
   the entire file as compressed literals (full transfer). This path is the
   correctness floor and always works (over direct or relay).

### Hash choice and collision tradeoff

MurmurHash3 x64 128 is fast and well-distributed but **not cryptographic**. Two
distinct 4 KB blocks collide with probability ≈ 2⁻¹²⁸ per pair; across ~10⁵ blocks
a false block match is astronomically unlikely. And even if one happened, it could
not corrupt the target: the reconstructed file's own whole-file 128-bit hash is
checked against the source's before anything is swapped, so a bad block match only
causes a **verify failure and an automatic retry** next launch. There is no
adversary here (your own two Macs, your own friends service), so a cryptographic
hash would buy nothing.

---

## Safety (it is a real message database)

- **Never written in place.** The file is reconstructed to `<target>.incoming`.
- **Verified before any swap.** The reconstructed file's size and whole-file 128-bit
  hash must equal the source's reported values. On mismatch the `.incoming` file is
  deleted, the real target is left **untouched**, the error is logged, and **no
  success marker is written** (so it retries next launch).
- **Backup + atomic swap.** Only on a verified match: any existing target is renamed
  to `<target>.bak-<yyyymmdd-hhmmss>` (kept, never deleted), then the `.incoming`
  file is atomically renamed into place (same directory → atomic rename). The
  previous database is always preserved as a timestamped backup.
- **Consistent snapshot on the source.** `chat.db` is a live SQLite database with a
  write-ahead log (`chat.db-wal`, `chat.db-shm`). The feature treats the file as
  **opaque bytes** and never interprets SQLite. The source **refuses to serve while
  the Messages app is running** (checked via the macOS process list) and logs a
  clear "quit Messages first" message; it also **warns** if a non-empty `chat.db-wal`
  sits beside the file (only `chat.db` itself is synced, so quit Messages so it
  checkpoints the WAL into `chat.db` first).
- **Friendship-gated.** Both the direct and relay paths require a relay ticket the
  friends service mints only after a friendship check, so the database is not served
  to strangers.
- **Bounded and validated.** Every delta message is length-prefixed and CRC-checked;
  sizes are validated against sane caps before any allocation; free space is checked
  before reconstruct; short reads, disconnects, and partial transfers fail cleanly
  without crashing or half-writing the target.
- **Dry run.** With `kFileSyncDryRun = true` the client does everything — discover,
  connect, negotiate the delta, reconstruct, and verify — **except** the final
  backup+swap. It logs the delta size and the percentage of bytes reused, deletes the
  `.incoming` file, and leaves the real target untouched (and writes no marker, so you
  can re-test).

---

## Isolation guarantees (why it can't break the game)

- Empty `kFileSyncSourceUsername` → zero code runs; no thread, no sockets.
- When on, it starts once at the title screen on its **own background thread(s)**,
  wrapped in catch-all handlers; any exception, error, or timeout is logged and the
  thread ends (or, on the source, keeps looping) — the game never sees it.
- It opens its **own dedicated** friends-service connection and never calls into the
  game's `FriendsClient`; it never changes the game's presence, never installs or
  overrides the game's relay socket handler, and never touches game state.
- It never blocks title-screen rendering or input. On shutdown the game calls
  `RequestStop()`, which signals the worker and joins with a short timeout, then
  **detaches** if the worker is still finishing — so shutdown is never held up.
- The delta engine is kept out of `DebugSystem` and pulls in no heavy game headers.
- The service change is additive: the two new ops live beside the existing ones and
  reuse the existing relay splice; the accounts/presence/join/hosting paths are
  untouched.

---

## Limitations

- Only `chat.db` itself is transferred, not `-wal`/`-shm`. Quit Messages on the
  source so it checkpoints the WAL into `chat.db` before serving.
- A trailing partial block on the client's base file is not signed (its bytes simply
  arrive as literals) — a negligible efficiency loss, never a correctness issue.
- Both Macs must be signed into the friends service and be friends, and the service
  must be running the updated `friends_service.py`.
- Role is decided by player name, so the new Mac must not sign in under the source
  account name (see "One implication to know" above).
- The direct path depends on the source's router honouring UPnP; if it does not, the
  relay path through the friends service is used automatically.
