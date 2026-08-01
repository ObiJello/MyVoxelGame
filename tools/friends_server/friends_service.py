#!/usr/bin/env python3
"""ObeyCraft friends service — accounts, friends graph, presence, invites.

Single-file, stdlib-only (Python 3.9+). Run it on the hosting Mac next to the
game server:

    python3 tools/friends_server/friends_service.py \
        --port 25570 --db ~/obeycraft-friends.db

Forward TCP 25570 on the router (alongside the game's 25565).

Two protocols share the one port, sniffed by the first byte of a connection:

  * '{'          -> persistent NDJSON session (the game client). First line
                    must be {"op":"hello","token":...,"proto":1}. The server
                    then pushes {"event":"roster",...} snapshots on every
                    change, {"event":"invite",...} messages, and
                    {"event":"relay_open","ticket":...} when a friend wants
                    to join through the relay. Requests carry an "id" echoed
                    on the matching response.
                    A first line of {"op":"relay_attach","role":...,
                    "ticket":...} instead turns the connection into one half
                    of a relayed game tunnel (see below).
  * ASCII letter -> one HTTP/1.1 request (the launcher). POST /api with a
                    JSON body {"op":...}; response is JSON; connection closes.

Every op returns {"ok": true, ...} or {"ok": false, "error": "<code>"}.

HOSTING WITHOUT PORT-FORWARDING: when a player reports state "hosting", the
service TCP-probes their advertised address to see whether their game port is
actually reachable (their game tries to open it automatically via UPnP). If it
is, join_info hands out the direct address and the two players talk straight to
each other. If it isn't, join_info mints a relay ticket, pushes relay_open to
the host, and both sides dial OUT to this service — outbound connections always
work — where their streams are spliced. Relay traffic rides the same port, so
only ONE port ever needs forwarding on the machine running this service.

SECURITY MODEL (read before exposing beyond friends): transport is PLAINTEXT
TCP/HTTP — no TLS. Passwords are scrypt-hashed at rest and session tokens are
random 128-bit values, but anything on-path can read traffic. This is sized
for a personal server shared with people you know, nothing more.

Optional launchd auto-start — save as
~/Library/LaunchAgents/com.obeycraft.friends.plist and `launchctl load` it:

    <?xml version="1.0" encoding="UTF-8"?>
    <!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN"
      "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
    <plist version="1.0"><dict>
      <key>Label</key><string>com.obeycraft.friends</string>
      <key>ProgramArguments</key><array>
        <string>/usr/bin/python3</string>
        <string>/Users/obey/Desktop/MyVoxelGame/tools/friends_server/friends_service.py</string>
        <string>--port</string><string>25570</string>
        <string>--db</string><string>/Users/obey/obeycraft-friends.db</string>
      </array>
      <key>RunAtLoad</key><true/>
      <key>KeepAlive</key><true/>
    </dict></plist>
"""

import argparse
import asyncio
import hashlib
import json
import logging
import re
import secrets
import sqlite3
import time

log = logging.getLogger("friends")

NAME_RE = re.compile(r"^[A-Za-z0-9_]{3,16}$")
MIN_PASSWORD_LEN = 4
PING_DROP_SECONDS = 90          # silent NDJSON connections dropped after this
OFFLINE_GRACE_SECONDS = 10      # disconnect -> offline broadcast delay
SCRYPT_N, SCRYPT_R, SCRYPT_P = 2 ** 14, 8, 1
PBKDF2_ITERATIONS = 200_000

# hashlib.scrypt needs OpenSSL 1.1+; Apple's system Python links LibreSSL and
# simply doesn't have it. Pick the best KDF this interpreter actually offers
# and record the choice per account, so a database stays readable if the
# service later runs under a different Python.
PREFERRED_KDF = "scrypt" if hasattr(hashlib, "scrypt") else "pbkdf2"

# ── Direct-vs-relay tuning ────────────────────────────────────────────────
PROBE_TIMEOUT_SECONDS = 3.0     # TCP connect test against a host's game port
PROBE_CACHE_SECONDS = 120.0     # re-probe if the cached verdict is older
RELAY_TICKET_TTL_SECONDS = 30   # unpaired relay tickets expire after this
RELAY_BUFFER = 64 * 1024

# Set by --allow-private-hosts: treat private/loopback host addresses as
# directly reachable (LAN-only deployments and local testing).
ALLOW_PRIVATE_HOSTS = False


# ═══════════════════════════════ storage ═══════════════════════════════════

class DB:
    """Synchronous sqlite — every handler completes in microseconds at the
    scale this serves, so no executor indirection."""

    def __init__(self, path: str):
        self.conn = sqlite3.connect(path)
        self.conn.row_factory = sqlite3.Row
        self.conn.execute("PRAGMA journal_mode=WAL")
        self.conn.execute("PRAGMA foreign_keys=ON")
        self.conn.executescript(
            """
            CREATE TABLE IF NOT EXISTS accounts(
                id      INTEGER PRIMARY KEY,
                name    TEXT NOT NULL UNIQUE COLLATE NOCASE,
                salt    BLOB NOT NULL,
                hash    BLOB NOT NULL,
                algo    TEXT NOT NULL DEFAULT 'scrypt',
                created INTEGER NOT NULL);
            CREATE TABLE IF NOT EXISTS sessions(
                token     TEXT PRIMARY KEY,
                account   INTEGER NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,
                created   INTEGER NOT NULL,
                last_seen INTEGER NOT NULL);
            CREATE TABLE IF NOT EXISTS friends(
                a INTEGER NOT NULL,
                b INTEGER NOT NULL,          -- canonical: a < b
                created INTEGER NOT NULL,
                PRIMARY KEY(a, b));
            CREATE TABLE IF NOT EXISTS requests(
                src INTEGER NOT NULL,
                dst INTEGER NOT NULL,
                created INTEGER NOT NULL,
                PRIMARY KEY(src, dst));
            """
        )
        # Migration: databases created before per-account KDF tagging.
        columns = {row[1] for row in
                   self.conn.execute("PRAGMA table_info(accounts)")}
        if "algo" not in columns:
            self.conn.execute("ALTER TABLE accounts ADD COLUMN algo TEXT "
                              "NOT NULL DEFAULT 'scrypt'")
        self.conn.commit()

    # ── accounts ────────────────────────────────────────────────────────

    @staticmethod
    def _hash(password: str, salt: bytes, algo: str) -> bytes:
        if algo == "scrypt":
            return hashlib.scrypt(password.encode(), salt=salt,
                                  n=SCRYPT_N, r=SCRYPT_R, p=SCRYPT_P)
        return hashlib.pbkdf2_hmac("sha256", password.encode(), salt,
                                   PBKDF2_ITERATIONS)

    def create_account(self, name: str, password: str) -> int:
        salt = secrets.token_bytes(16)
        cur = self.conn.execute(
            "INSERT INTO accounts(name, salt, hash, algo, created)"
            " VALUES(?,?,?,?,?)",
            (name, salt, self._hash(password, salt, PREFERRED_KDF),
             PREFERRED_KDF, int(time.time())))
        self.conn.commit()
        return cur.lastrowid

    def find_account(self, name: str):
        return self.conn.execute(
            "SELECT * FROM accounts WHERE name=? COLLATE NOCASE", (name,)
        ).fetchone()

    def account_by_id(self, account_id: int):
        return self.conn.execute(
            "SELECT * FROM accounts WHERE id=?", (account_id,)).fetchone()

    def verify_password(self, row, password: str) -> bool:
        # Use the algorithm this account was created with, not the current
        # preference — otherwise moving between interpreters locks people out.
        algo = row["algo"] if "algo" in row.keys() and row["algo"] else "scrypt"
        if algo == "scrypt" and not hasattr(hashlib, "scrypt"):
            log.error("account '%s' was hashed with scrypt, which this Python "
                      "lacks — run the service with a Python built against "
                      "OpenSSL, or recreate the account", row["name"])
            return False
        return secrets.compare_digest(self._hash(password, row["salt"], algo),
                                      row["hash"])

    def rename(self, account_id: int, name: str):
        self.conn.execute("UPDATE accounts SET name=? WHERE id=?",
                          (name, account_id))
        self.conn.commit()

    def set_password(self, account_id: int, password: str):
        salt = secrets.token_bytes(16)
        self.conn.execute(
            "UPDATE accounts SET salt=?, hash=?, algo=? WHERE id=?",
            (salt, self._hash(password, salt, PREFERRED_KDF), PREFERRED_KDF,
             account_id))
        self.conn.commit()

    def delete_other_sessions(self, account_id: int, keep_token: str):
        """Revoke every session for the account except `keep_token`."""
        self.conn.execute(
            "DELETE FROM sessions WHERE account=? AND token<>?",
            (account_id, keep_token))
        self.conn.commit()

    # ── sessions ────────────────────────────────────────────────────────

    def create_session(self, account_id: int) -> str:
        token = secrets.token_hex(16)
        now = int(time.time())
        self.conn.execute(
            "INSERT INTO sessions(token, account, created, last_seen)"
            " VALUES(?,?,?,?)", (token, account_id, now, now))
        self.conn.commit()
        return token

    def session_account(self, token: str):
        """token -> account row, or None."""
        row = self.conn.execute(
            "SELECT account FROM sessions WHERE token=?", (token,)).fetchone()
        if row is None:
            return None
        self.conn.execute("UPDATE sessions SET last_seen=? WHERE token=?",
                          (int(time.time()), token))
        return self.account_by_id(row["account"])

    def delete_session(self, token: str):
        self.conn.execute("DELETE FROM sessions WHERE token=?", (token,))
        self.conn.commit()

    # ── friends graph ───────────────────────────────────────────────────

    @staticmethod
    def _pair(x: int, y: int):
        return (x, y) if x < y else (y, x)

    def are_friends(self, x: int, y: int) -> bool:
        a, b = self._pair(x, y)
        return self.conn.execute(
            "SELECT 1 FROM friends WHERE a=? AND b=?", (a, b)).fetchone() is not None

    def add_friendship(self, x: int, y: int):
        a, b = self._pair(x, y)
        self.conn.execute(
            "INSERT OR IGNORE INTO friends(a, b, created) VALUES(?,?,?)",
            (a, b, int(time.time())))
        self.conn.commit()

    def remove_friendship(self, x: int, y: int):
        a, b = self._pair(x, y)
        self.conn.execute("DELETE FROM friends WHERE a=? AND b=?", (a, b))
        self.conn.commit()

    def friends_of(self, account_id: int):
        rows = self.conn.execute(
            """SELECT acc.id, acc.name FROM friends f
               JOIN accounts acc ON acc.id = CASE WHEN f.a=? THEN f.b ELSE f.a END
               WHERE f.a=? OR f.b=? ORDER BY acc.name COLLATE NOCASE""",
            (account_id, account_id, account_id)).fetchall()
        return [(r["id"], r["name"]) for r in rows]

    def request_exists(self, src: int, dst: int) -> bool:
        return self.conn.execute(
            "SELECT 1 FROM requests WHERE src=? AND dst=?", (src, dst)
        ).fetchone() is not None

    def add_request(self, src: int, dst: int):
        self.conn.execute(
            "INSERT OR IGNORE INTO requests(src, dst, created) VALUES(?,?,?)",
            (src, dst, int(time.time())))
        self.conn.commit()

    def remove_request(self, src: int, dst: int):
        self.conn.execute("DELETE FROM requests WHERE src=? AND dst=?",
                          (src, dst))
        self.conn.commit()

    def requests_incoming(self, account_id: int):
        rows = self.conn.execute(
            """SELECT acc.id, acc.name FROM requests r
               JOIN accounts acc ON acc.id = r.src WHERE r.dst=?
               ORDER BY r.created""", (account_id,)).fetchall()
        return [(r["id"], r["name"]) for r in rows]

    def requests_outgoing(self, account_id: int):
        rows = self.conn.execute(
            """SELECT acc.id, acc.name FROM requests r
               JOIN accounts acc ON acc.id = r.dst WHERE r.src=?
               ORDER BY r.created""", (account_id,)).fetchall()
        return [(r["id"], r["name"]) for r in rows]


# ═══════════════════════════════ presence ══════════════════════════════════

class Presence:
    """In-memory presence per account. state: menu|playing|hosting.
    Lost on restart — clients reconnect and re-report."""

    def __init__(self):
        self.online = {}     # account_id -> dict(state, world, host, port, conns:set)
        self.offline_tasks = {}  # account_id -> asyncio.Task (grace timers)

    def touch(self, account_id: int, conn):
        entry = self.online.setdefault(account_id, {
            "state": "menu", "world": "", "host": "", "port": 0,
            # Direct-reachability: None = not probed yet, True/False = result.
            # False means joiners get relayed instead.
            "direct_ok": None, "probed_at": 0.0,
            "conns": set()})
        entry["conns"].add(conn)
        task = self.offline_tasks.pop(account_id, None)
        if task:
            task.cancel()
        return entry

    def update(self, account_id: int, state: str, world: str,
               host: str, port: int):
        entry = self.online.get(account_id)
        if entry is None:
            return
        # Any change to where/whether we're hosting invalidates the probe.
        if (entry["state"] != state or entry["host"] != host
                or entry["port"] != port):
            entry["direct_ok"] = None
            entry["probed_at"] = 0.0
        entry.update(state=state, world=world, host=host, port=port)

    def drop_conn(self, account_id: int, conn) -> bool:
        """Returns True when this was the account's last connection."""
        entry = self.online.get(account_id)
        if entry is None:
            return False
        entry["conns"].discard(conn)
        return not entry["conns"]

    def remove(self, account_id: int):
        self.online.pop(account_id, None)
        self.offline_tasks.pop(account_id, None)

    def snapshot(self, account_id: int) -> dict:
        entry = self.online.get(account_id)
        if entry is None:
            return {"state": "offline", "world": ""}
        return {"state": entry["state"], "world": entry["world"]}

    def host_info(self, account_id: int):
        entry = self.online.get(account_id)
        if entry is None or entry["state"] != "hosting":
            return None
        return entry["host"], entry["port"], entry["world"]

    def entry(self, account_id: int):
        return self.online.get(account_id)


# ═══════════════════════ reachability + relay ══════════════════════════════

def is_public_ip(ip: str) -> bool:
    """True when `ip` is an address other machines on the internet could
    actually dial. Rejects loopback / RFC1918 / CGNAT / link-local, which is
    what we see when the host shares a LAN with the service (or sits behind
    carrier-grade NAT) — those must be relayed, not advertised.

    --allow-private-hosts flips this off for LAN-only / local testing, where
    private addresses ARE the reachable ones."""
    if ALLOW_PRIVATE_HOSTS:
        return bool(ip)
    parts = ip.split(".")
    if len(parts) != 4:
        return False   # IPv6 or malformed: don't advertise
    try:
        a, b = int(parts[0]), int(parts[1])
    except ValueError:
        return False
    if a == 10 or a == 127 or a == 0:
        return False
    if a == 172 and 16 <= b <= 31:
        return False
    if a == 192 and b == 168:
        return False
    if a == 169 and b == 254:
        return False
    if a == 100 and 64 <= b <= 127:   # CGNAT
        return False
    return True


async def probe_reachable(host: str, port: int) -> bool:
    """Can we open a TCP connection to the host's game port from out here?
    This is the ground truth for 'their UPnP mapping actually works' — the
    host claiming success isn't enough (double-NAT, ISP filtering)."""
    if not host or not port:
        return False
    try:
        fut = asyncio.open_connection(host, port)
        reader, writer = await asyncio.wait_for(fut, PROBE_TIMEOUT_SECONDS)
        writer.close()
        try:
            await writer.wait_closed()
        except Exception:  # noqa: BLE001
            pass
        return True
    except Exception:  # noqa: BLE001 — any failure means "not reachable"
        return False


class Relay:
    """Ticket-paired TCP splicing so friends behind unfriendly routers can
    still host. Both sides dial OUT to this service (always allowed), then
    we pipe the two streams together:

        joiner --\\                       /-- host game server
                  >-- [this service] ---<
        (attach)  /                       \\  (attach, after relay_open push)

    Runs on the SAME port as everything else — the attach handshake is just
    a first NDJSON line with op "relay_attach", so no extra port-forwarding.
    """

    def __init__(self):
        self.tickets = {}   # ticket -> dict(host_id, waiter, streams, created)

    def create(self, host_id: int, joiner_id: int) -> str:
        ticket = secrets.token_hex(16)
        self.tickets[ticket] = {
            "host_id": host_id, "joiner_id": joiner_id,
            "created": time.time(),
            "host": None, "joiner": None,
            "ready": asyncio.Event(),   # both halves attached
            "done": asyncio.Event(),    # splice finished
        }
        self.sweep()
        return ticket

    def sweep(self):
        now = time.time()
        for ticket, slot in list(self.tickets.items()):
            if now - slot["created"] > RELAY_TICKET_TTL_SECONDS and \
                    not (slot["host"] and slot["joiner"]):
                self.tickets.pop(ticket, None)

    async def attach(self, ticket: str, role: str,
                     reader: asyncio.StreamReader,
                     writer: asyncio.StreamWriter) -> bool:
        slot = self.tickets.get(ticket)
        if slot is None or role not in ("host", "joiner"):
            return False
        if slot[role] is not None:
            return False          # duplicate attach
        slot[role] = (reader, writer)
        writer.write(b'{"ok":true}\n')
        await writer.drain()

        if slot["host"] and slot["joiner"]:
            slot["ready"].set()          # partner already waiting
        else:
            try:
                await asyncio.wait_for(slot["ready"].wait(),
                                       RELAY_TICKET_TTL_SECONDS)
            except asyncio.TimeoutError:
                self.tickets.pop(ticket, None)
                return False

        # BOTH attach calls must stay alive for the life of the tunnel —
        # returning early would let the connection handler close that half's
        # socket mid-session. The host side drives the copy loops; the joiner
        # side parks until they finish.
        if role != "host":
            await slot["done"].wait()
            return True

        self.tickets.pop(ticket, None)
        hr, hw = slot["host"]
        jr, jw = slot["joiner"]
        log.info("relay: splicing session (host id %d <-> joiner id %d)",
                 slot["host_id"], slot["joiner_id"])
        try:
            await self.splice(hr, hw, jr, jw)
        finally:
            slot["done"].set()
        return True

    @staticmethod
    async def splice(hr, hw, jr, jw):
        async def pump(reader, writer):
            try:
                while True:
                    data = await reader.read(RELAY_BUFFER)
                    if not data:
                        break
                    writer.write(data)
                    await writer.drain()
            except Exception:  # noqa: BLE001
                pass
            finally:
                try:
                    writer.close()
                except Exception:  # noqa: BLE001
                    pass

        await asyncio.gather(pump(hr, jw), pump(jr, hw))


# ═══════════════════════════════ service ═══════════════════════════════════

class Service:
    def __init__(self, db: DB):
        self.db = db
        self.presence = Presence()
        self.relay = Relay()

    # ── event push ──────────────────────────────────────────────────────

    def roster_payload(self, account_id: int) -> dict:
        friends = [{"id": fid, "name": fname,
                    "presence": self.presence.snapshot(fid)}
                   for fid, fname in self.db.friends_of(account_id)]
        incoming = [{"id": i, "name": n}
                    for i, n in self.db.requests_incoming(account_id)]
        outgoing = [{"id": i, "name": n}
                    for i, n in self.db.requests_outgoing(account_id)]
        return {"friends": friends, "incoming": incoming, "outgoing": outgoing}

    def push(self, account_id: int, message: dict):
        entry = self.presence.online.get(account_id)
        if not entry:
            return
        line = (json.dumps(message, separators=(",", ":")) + "\n").encode()
        for conn in list(entry["conns"]):
            conn.send_line(line)

    def push_roster(self, account_id: int):
        self.push(account_id, {"event": "roster",
                               **self.roster_payload(account_id)})

    def push_roster_to_friends(self, account_id: int):
        for fid, _ in self.db.friends_of(account_id):
            self.push_roster(fid)

    def broadcast_change(self, *account_ids):
        """Push fresh rosters to the given accounts AND everyone friended
        with them (presence/name changes affect both sides)."""
        seen = set()
        for aid in account_ids:
            for target in [aid] + [f for f, _ in self.db.friends_of(aid)]:
                if target not in seen:
                    seen.add(target)
                    self.push_roster(target)

    # ── op dispatch (shared by HTTP and NDJSON) ─────────────────────────

    async def handle_op(self, body: dict, peer_ip: str, conn=None) -> dict:
        op = body.get("op", "")
        handler = getattr(self, f"op_{op}", None)
        if handler is None:
            return {"ok": False, "error": "unknown_op"}
        try:
            # Most ops are plain functions; the ones that touch the network
            # (join_info probes reachability) are coroutines.
            if asyncio.iscoroutinefunction(handler):
                return await handler(body, peer_ip, conn)
            return handler(body, peer_ip, conn)
        except Exception:  # noqa: BLE001 — one bad request must not kill the loop
            log.exception("op %s failed", op)
            return {"ok": False, "error": "internal"}

    def _auth(self, body: dict, conn=None):
        token = body.get("token", "")
        if token:
            return self.db.session_account(token)
        # NDJSON sessions authenticated at hello don't re-send the token —
        # the connection itself carries the identity.
        if conn is not None and conn.account_id is not None:
            return self.db.account_by_id(conn.account_id)
        return None

    # ── account ops ─────────────────────────────────────────────────────

    def op_signup(self, body, peer_ip, conn):
        name = str(body.get("name", ""))
        password = str(body.get("password", ""))
        if not NAME_RE.match(name):
            return {"ok": False, "error": "name_invalid"}
        if len(password) < MIN_PASSWORD_LEN:
            return {"ok": False, "error": "password_too_short"}
        if self.db.find_account(name) is not None:
            return {"ok": False, "error": "name_taken"}
        account_id = self.db.create_account(name, password)
        token = self.db.create_session(account_id)
        log.info("signup: %s (id %d) from %s", name, account_id, peer_ip)
        return {"ok": True, "account_id": account_id, "token": token,
                "name": name, "created": int(time.time())}

    def op_login(self, body, peer_ip, conn):
        row = self.db.find_account(str(body.get("name", "")))
        if row is None or not self.db.verify_password(
                row, str(body.get("password", ""))):
            return {"ok": False, "error": "bad_credentials"}
        token = self.db.create_session(row["id"])
        log.info("login: %s (id %d) from %s", row["name"], row["id"], peer_ip)
        return {"ok": True, "account_id": row["id"], "token": token,
                "name": row["name"], "created": row["created"]}

    def op_logout(self, body, peer_ip, conn):
        self.db.delete_session(str(body.get("token", "")))
        return {"ok": True}

    def op_check_name(self, body, peer_ip, conn):
        name = str(body.get("name", ""))
        if not NAME_RE.match(name):
            return {"ok": True, "status": "invalid"}
        row = self.db.find_account(name)
        if row is None:
            return {"ok": True, "status": "available"}
        me = self._auth(body, conn)
        if me is not None and me["id"] == row["id"]:
            return {"ok": True, "status": "yours"}
        return {"ok": True, "status": "taken"}

    def op_change_password(self, body, peer_ip, conn):
        me = self._auth(body, conn)
        if me is None:
            return {"ok": False, "error": "bad_token"}
        if not self.db.verify_password(me, str(body.get("current", ""))):
            return {"ok": False, "error": "bad_credentials"}
        password = str(body.get("password", ""))
        if len(password) < MIN_PASSWORD_LEN:
            return {"ok": False, "error": "password_too_short"}
        self.db.set_password(me["id"], password)
        # Kick every other device off the account; the requesting session stays.
        self.db.delete_other_sessions(me["id"], str(body.get("token", "")))
        log.info("change_password: %s (id %d) from %s",
                 me["name"], me["id"], peer_ip)
        return {"ok": True}

    def op_rename(self, body, peer_ip, conn):
        me = self._auth(body, conn)
        if me is None:
            return {"ok": False, "error": "bad_token"}
        name = str(body.get("name", ""))
        if not NAME_RE.match(name):
            return {"ok": False, "error": "name_invalid"}
        existing = self.db.find_account(name)
        if existing is not None and existing["id"] != me["id"]:
            return {"ok": False, "error": "name_taken"}
        self.db.rename(me["id"], name)
        log.info("rename: id %d -> %s", me["id"], name)
        # Friendships key on account id, so friends see the new name as soon
        # as their next roster arrives — push it now.
        self.broadcast_change(me["id"])
        return {"ok": True, "name": name}

    # ── friends ops ─────────────────────────────────────────────────────

    def op_roster(self, body, peer_ip, conn):
        me = self._auth(body, conn)
        if me is None:
            return {"ok": False, "error": "bad_token"}
        return {"ok": True, **self.roster_payload(me["id"])}

    def op_friend_request(self, body, peer_ip, conn):
        me = self._auth(body, conn)
        if me is None:
            return {"ok": False, "error": "bad_token"}
        target = self.db.find_account(str(body.get("name", "")))
        if target is None:
            return {"ok": False, "error": "not_found"}
        if target["id"] == me["id"]:
            return {"ok": False, "error": "self"}
        if self.db.are_friends(me["id"], target["id"]):
            return {"ok": False, "error": "already_friends"}
        if self.db.request_exists(me["id"], target["id"]):
            return {"ok": False, "error": "already_pending"}
        if self.db.request_exists(target["id"], me["id"]):
            # They already asked us — mutual intent, auto-accept.
            self.db.remove_request(target["id"], me["id"])
            self.db.add_friendship(me["id"], target["id"])
        else:
            self.db.add_request(me["id"], target["id"])
        self.broadcast_change(me["id"], target["id"])
        return {"ok": True}

    # NOTE: the target account travels in "friend" — NEVER "id", which is the
    # NDJSON request-correlation field (they collided in the first draft).
    def _resolve_request(self, body, conn, accept: bool):
        me = self._auth(body, conn)
        if me is None:
            return {"ok": False, "error": "bad_token"}
        other = int(body.get("friend", 0))
        if not self.db.request_exists(other, me["id"]):
            return {"ok": False, "error": "no_request"}
        self.db.remove_request(other, me["id"])
        if accept:
            self.db.add_friendship(me["id"], other)
        self.broadcast_change(me["id"], other)
        return {"ok": True}

    def op_friend_accept(self, body, peer_ip, conn):
        return self._resolve_request(body, conn, accept=True)

    def op_friend_decline(self, body, peer_ip, conn):
        return self._resolve_request(body, conn, accept=False)

    def op_friend_remove(self, body, peer_ip, conn):
        me = self._auth(body, conn)
        if me is None:
            return {"ok": False, "error": "bad_token"}
        other = int(body.get("friend", 0))
        self.db.remove_friendship(me["id"], other)
        # Also clear any dangling request in either direction.
        self.db.remove_request(me["id"], other)
        self.db.remove_request(other, me["id"])
        self.broadcast_change(me["id"], other)
        return {"ok": True}

    # ── presence / invites / join ───────────────────────────────────────

    def op_presence(self, body, peer_ip, conn):
        me = self._auth(body, conn)
        if me is None:
            return {"ok": False, "error": "bad_token"}
        state = str(body.get("state", "menu"))
        if state not in ("menu", "playing", "hosting"):
            return {"ok": False, "error": "bad_state"}
        world = str(body.get("world", ""))[:48]
        port = int(body.get("port", 0)) if state == "hosting" else 0

        # Which address should joiners dial? Normally the observed source
        # address. When the host shares a LAN with this service (or sits
        # behind CGNAT) that address is private and useless to the outside,
        # so fall back to the external IP their UPnP mapping reported.
        host = ""
        if state == "hosting":
            host = peer_ip if is_public_ip(peer_ip) else ""
            reported = str(body.get("external_ip", ""))[:45]
            if not host and is_public_ip(reported):
                host = reported

        self.presence.update(me["id"], state, world, host, port)
        # Verify reachability out-of-band; joiners fall back to the relay
        # until (and unless) the probe succeeds.
        if state == "hosting":
            asyncio.get_running_loop().create_task(
                self.refresh_direct(me["id"]))
        self.broadcast_change(me["id"])
        return {"ok": True}

    async def refresh_direct(self, account_id: int):
        """Probe the host's advertised address and cache the verdict."""
        entry = self.presence.entry(account_id)
        if entry is None or entry["state"] != "hosting":
            return
        host, port = entry["host"], entry["port"]
        ok = await probe_reachable(host, port) if host else False
        entry = self.presence.entry(account_id)   # may have changed meanwhile
        if entry is None or entry["state"] != "hosting":
            return
        if entry["host"] != host or entry["port"] != port:
            return
        entry["direct_ok"] = ok
        entry["probed_at"] = time.time()
        log.info("probe: account %d at %s:%s -> %s", account_id, host or "?",
                 port, "DIRECT" if ok else "relay")

    async def resolve_join(self, joiner_id: int, host_id: int) -> dict:
        """Pick direct or relay for this join, refreshing a stale probe."""
        entry = self.presence.entry(host_id)
        if entry is None or entry["state"] != "hosting":
            return {"ok": False, "error": "not_hosting"}
        stale = (entry["direct_ok"] is None or
                 time.time() - entry["probed_at"] > PROBE_CACHE_SECONDS)
        if stale:
            await self.refresh_direct(host_id)
            entry = self.presence.entry(host_id)
            if entry is None or entry["state"] != "hosting":
                return {"ok": False, "error": "not_hosting"}

        if entry["direct_ok"]:
            return {"ok": True, "mode": "direct", "host": entry["host"],
                    "port": entry["port"], "world": entry["world"]}

        # Relay: mint a ticket and tell the host to dial in. The joiner
        # connects to this same service address (it already knows it).
        ticket = self.relay.create(host_id, joiner_id)
        self.push(host_id, {"event": "relay_open", "ticket": ticket,
                            "port": entry["port"]})
        return {"ok": True, "mode": "relay", "ticket": ticket,
                "world": entry["world"], "host": "", "port": 0}

    def op_invite(self, body, peer_ip, conn):
        me = self._auth(body, conn)
        if me is None:
            return {"ok": False, "error": "bad_token"}
        other = int(body.get("friend", 0))
        if not self.db.are_friends(me["id"], other):
            return {"ok": False, "error": "not_friends"}
        info = self.presence.host_info(me["id"])
        if info is None:
            return {"ok": False, "error": "not_hosting"}
        if self.presence.online.get(other) is None:
            return {"ok": False, "error": "not_online"}
        _, _, world = info
        # No address here on purpose: the invitee resolves it with join_info
        # when they actually click Join, so direct-vs-relay is decided fresh.
        self.push(other, {"event": "invite",
                          "from": {"id": me["id"], "name": me["name"]},
                          "world": world})
        return {"ok": True}

    async def op_join_info(self, body, peer_ip, conn):
        me = self._auth(body, conn)
        if me is None:
            return {"ok": False, "error": "bad_token"}
        other = int(body.get("friend", 0))
        if not self.db.are_friends(me["id"], other):
            return {"ok": False, "error": "not_friends"}
        return await self.resolve_join(me["id"], other)

    def op_ping(self, body, peer_ip, conn):
        return {"ok": True}


# ═══════════════════════════════ front-ends ════════════════════════════════

class NdjsonConn:
    """One live game connection. send_line is safe to call from handlers."""

    def __init__(self, writer: asyncio.StreamWriter):
        self.writer = writer
        self.account_id = None

    def send_line(self, line: bytes):
        try:
            self.writer.write(line)
        except Exception:  # noqa: BLE001
            pass


async def serve_ndjson(service: Service, first: bytes,
                       reader: asyncio.StreamReader,
                       writer: asyncio.StreamWriter, peer_ip: str):
    conn = NdjsonConn(writer)

    async def read_line() -> dict:
        rest = await asyncio.wait_for(reader.readline(), PING_DROP_SECONDS)
        if not rest:
            raise ConnectionError("eof")
        return json.loads((first_buf.pop() if first_buf else b"") + rest)

    # `first` byte was consumed by the sniffer — prepend it to line one only.
    first_buf = [first]

    # Handshake.
    hello = await read_line()

    # Relay attach: this connection is not a control session at all — it's
    # one half of a game tunnel. Hand it to the relay, which splices it to
    # its partner and owns the socket from here on.
    if hello.get("op") == "relay_attach":
        ok = await service.relay.attach(str(hello.get("ticket", "")),
                                        str(hello.get("role", "")),
                                        reader, writer)
        if not ok:
            try:
                writer.write(b'{"ok":false,"error":"bad_ticket"}\n')
                await writer.drain()
            except Exception:  # noqa: BLE001
                pass
        return

    account = service.db.session_account(str(hello.get("token", ""))) \
        if hello.get("op") == "hello" else None
    if account is None:
        writer.write(b'{"ok":false,"error":"bad_token"}\n')
        await writer.drain()
        return
    conn.account_id = account["id"]
    service.presence.touch(account["id"], conn)
    writer.write((json.dumps({
        "ok": True, "id": hello.get("id"),
        "account_id": account["id"], "name": account["name"],
    }, separators=(",", ":")) + "\n").encode())
    service.push_roster(account["id"])
    service.broadcast_change(account["id"])
    log.info("game online: %s (id %d) from %s",
             account["name"], account["id"], peer_ip)

    try:
        while True:
            body = await read_line()
            response = await service.handle_op(body, peer_ip, conn)
            response["id"] = body.get("id")
            writer.write((json.dumps(response, separators=(",", ":"))
                          + "\n").encode())
            await writer.drain()
    except (ConnectionError, asyncio.TimeoutError,
            asyncio.IncompleteReadError, json.JSONDecodeError):
        pass
    finally:
        if service.presence.drop_conn(account["id"], conn):
            # Last connection gone: grace period before the offline broadcast
            # so brief reconnects don't flap friends' rosters.
            async def go_offline(aid=account["id"]):
                await asyncio.sleep(OFFLINE_GRACE_SECONDS)
                service.presence.remove(aid)
                service.broadcast_change(aid)
                log.info("offline: id %d", aid)
            service.presence.offline_tasks[account["id"]] = \
                asyncio.get_running_loop().create_task(go_offline())


async def serve_http(service: Service, first: bytes,
                     reader: asyncio.StreamReader,
                     writer: asyncio.StreamWriter, peer_ip: str):
    """Minimal HTTP/1.1: one POST /api request, JSON in, JSON out, close."""
    try:
        head = first + await asyncio.wait_for(
            reader.readuntil(b"\r\n\r\n"), 10)
    except (asyncio.TimeoutError, asyncio.IncompleteReadError):
        return

    request_line = head.split(b"\r\n", 1)[0].decode("latin-1", "replace")
    content_length = 0
    for line in head.split(b"\r\n")[1:]:
        if line.lower().startswith(b"content-length:"):
            try:
                content_length = min(int(line.split(b":", 1)[1].strip()),
                                     64 * 1024)
            except ValueError:
                pass

    body_bytes = await reader.readexactly(content_length) \
        if content_length else b""

    status, payload = "200 OK", {"ok": False, "error": "bad_request"}
    parts = request_line.split(" ")
    if len(parts) >= 2 and parts[0] == "POST" and parts[1] == "/api":
        try:
            payload = await service.handle_op(json.loads(body_bytes or b"{}"),
                                              peer_ip, None)
        except json.JSONDecodeError:
            status = "400 Bad Request"
    else:
        status = "404 Not Found"
        payload = {"ok": False, "error": "not_found"}

    body = json.dumps(payload, separators=(",", ":")).encode()
    writer.write(
        f"HTTP/1.1 {status}\r\nContent-Type: application/json\r\n"
        f"Content-Length: {len(body)}\r\nConnection: close\r\n\r\n".encode()
        + body)
    await writer.drain()


async def serve(service: Service, reader: asyncio.StreamReader,
                writer: asyncio.StreamWriter):
    peer = writer.get_extra_info("peername")
    peer_ip = peer[0] if peer else "?"
    try:
        first = await asyncio.wait_for(reader.readexactly(1), 15)
        if first == b"{":
            await serve_ndjson(service, first, reader, writer, peer_ip)
        else:
            await serve_http(service, first, reader, writer, peer_ip)
    except (asyncio.TimeoutError, asyncio.IncompleteReadError,
            ConnectionError, OSError):
        pass
    finally:
        try:
            writer.close()
            await writer.wait_closed()
        except Exception:  # noqa: BLE001
            pass


async def main():
    parser = argparse.ArgumentParser(description="ObeyCraft friends service")
    parser.add_argument("--port", type=int, default=25570)
    parser.add_argument("--db", default="obeycraft-friends.db")
    parser.add_argument("--bind", default="0.0.0.0")
    parser.add_argument("--allow-private-hosts", action="store_true",
                        help="treat private/loopback host addresses as "
                             "directly joinable (LAN-only setups, testing)")
    args = parser.parse_args()

    global ALLOW_PRIVATE_HOSTS
    ALLOW_PRIVATE_HOSTS = args.allow_private_hosts

    logging.basicConfig(level=logging.INFO,
                        format="%(asctime)s %(levelname)s %(message)s")
    service = Service(DB(args.db))
    server = await asyncio.start_server(
        lambda r, w: serve(service, r, w), args.bind, args.port)
    log.info("friends service listening on %s:%d (db: %s, kdf: %s)",
             args.bind, args.port, args.db, PREFERRED_KDF)
    async with server:
        await server.serve_forever()


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
