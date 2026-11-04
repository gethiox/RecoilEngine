# Joining a running game from a snapshot

Without this feature a client (re)joining a running game receives every packet
of the game since its start and re-simulates the complete game ("catching
up"), which takes minutes in long games. With it, the joining client loads a
snapshot of the current game state provided by other clients and only
simulates the few frames that passed while the snapshot was transferred.

The joining client still receives the complete game history: the part before
the snapshot arrives in the background while it already plays and is inserted
into its demo, so its local replay is complete.

## Requirements

* **Exactness.** A client that joined from a snapshot must continue exactly
  like the clients that played from the start. Every byte of synced state has
  to be restored, including internal layouts that influence iteration order
  (hash maps, Lua tables, ECS storages) and pending work (path searches running
  in the background, map damage queues, LOS update queues).
* **No impact on other players.** Only the clients asked to provide the
  snapshot ("sources") briefly stall while serializing it. Spectators are
  preferred as sources since they do not influence the game speed; a joining
  spectator never stalls a player.
* **Never worse than before.** Every failure (no source, timeout,
  disconnect, digest mismatch, unsupported path finder, the joiner hosting a
  native AI) falls back to the classic catch-up.
* **Complete replay.** The joining client records the complete game history
  in its demo, with the original timing.

## Protocol

```
joiner               server                            sources (synced clients)
  |-- ATTEMPTCONNECT(snapshotJoin=1) -->|
  |<-- GAMEDATA ------------------------|
  |                                     |-- SNAPSHOT_REQUEST(rid, PROVIDER) --> source 1
  |                                     |-- SNAPSHOT_REQUEST(rid, VERIFIER) --> source 2..N
  |                                     |== SNAPSHOT_REQUEST(rid, MARKER, frame) ==> all (game stream)
  |                                     |                                   [snapshot at the marker]
  |                                     |<-- SNAPSHOT_HASH(digest) ------- every source
  |                                     |<-- SNAPSHOT_DATA(chunks) ------- provider
  |<-- SNAPSHOT_DATA(chunks, relayed) --|   (streamed while it arrives)
  |<-- SNAPSHOT_HASH(OK|ABORTED) -------|   (after all digests agree)
  |<-- SETPLAYERNUM, progress ----------|
  |<-- game stream from the MARKER on --|   (CATCHUP, then live packets)
  |<-- SNAPSHOT_HISTORY(chunks) --------|   (stream before the marker, in the background)
```

A join passes through three phases on the server (`CSnapshotJoinServer`):

1. **SNAPSHOT.** The joiner is held back: it receives no game packets
   (`GameParticipant::awaitingSnapshot`) while the snapshot is created,
   verified and relayed.
2. **CATCHUP.** The joiner receives the cached game stream starting at the
   marker, still held back from live broadcasts; once it has every cached
   packet it is released, so nothing can be missed or reordered. It only
   simulates the frames that passed since the marker.
3. **HISTORY.** The joiner plays live. The game stream before the marker,
   which it only needs for its demo, is sent as `SNAPSHOT_HISTORY` chunks
   (a byte stream of `[uint32 length][packet]` records) whenever the link is
   otherwise idle.

Details:

* **The snapshot point is a marker in the game stream**, not a frame number:
  packets between two frames (commands, Lua messages, ...) are applied as soon
  as they are read, so "after frame F" would be ambiguous. Sources snapshot
  exactly when they process the marker, and the joiner continues exactly
  after it. This also works while the game is paused.
* Role assignments are sent before the marker on the same ordered link. Other
  clients and replays ignore markers they were not assigned to.
* Sources serialize synchronously (the only stall), then hash in a background
  thread; the provider also compresses there and uploads in the background.
  The digest is of the uncompressed snapshot, so verifiers never compress. A
  client that is already creating a snapshot is not selected again; a request
  the server aborts never makes a source wait for its running compression.
* All bulk transfers (snapshot upload and relay, catch-up, history) are flow
  controlled: at most 32 kB are queued or unacknowledged on a link, so regular
  game packets are never stuck behind bulk data, and the reliable UDP protocol
  stays within its resend window (256 chunks).
* The server only reports success after every verifier's digest equals the
  provider's and at least `SnapshotJoinMinSources` sources (the provider plus
  confirming verifiers) stand behind the snapshot; verifiers that failed or
  left do not count. The joiner checks the digest again after decompressing.
* Snapshot transfers and the data sent to a joiner use `SnapshotTransferRate`
  instead of the regular per-link bandwidth limit, and snapshot packets are
  exempt from the server's anti-spam limits.
* The server does not expect sync responses from the joiner for frames covered
  by its snapshot (`GameParticipant::syncCheckStartFrame`); from the next frame
  on it is checked like every other client.
* On failure the joiner is told to catch up and the server sends it the
  complete game stream (also flow controlled) before releasing it.

## Snapshot contents

A snapshot is the creg saved-game state (see `GameSnapshot`), made canonical
so identical synced states give identical bytes on every client, which is
what lets verifiers confirm the provider's snapshot by digest:

* client specific data is excluded or stored as neutral values: local player
  identity, UI settings and caches, control groups, rendering data, native AI
  data, members marked `CR_MEMBER_UNSYNCED` (draw positions, feature fade
  values, cached model piece transforms, the local player's input statistics,
  icons and selection volumes set by widgets, visual smoke trails);
* unsynced objects (visual-effect projectiles, lights, the local selection)
  are left out, including every reference to them, and get sync-ids from a
  separate range so synced objects get identical ids everywhere;
* the synced Lua heap is brought into a canonical form that does not change
  its behaviour (`creg::MakeCanonicalLuaState`): leaked values on idle
  thread stacks dropped, sizes that depend on the collector's (wall-clock
  driven) timing normalized, string table buckets sorted, and tables whose
  size and node layout depend on the collector's timing (weak tables, tables
  keyed by coroutines/tables/functions, e.g. BAR's per-unit script thread
  sets) resized and rebuilt from their contents in a canonical order;
* a path whose owning unit was destroyed before the path manager dropped it
  (the engine tolerates such owners and only checks that they are unusable)
  gets a stand-in dead owner on the joining client;
* per-query stamps (`tempNum`) are written canonically.

Loading keeps the joining client's identity and unsynced settings. Nothing is
recomputed on load: everything the simulation depends on is stored exactly,
including derived data that is updated lazily or paced over frames (height
map derivatives, LOS, path finder, smooth height mesh, explosions still
deforming the terrain, terrain type properties changed by Lua) and the
storage order of every container that is iterated (hash maps, Lua tables, ECS
storages); see the "Savegame:" commits. Unsynced data a joining client needs
(selection volumes, piece transforms) is re-initialized on load.

## Opting in

Joining from a snapshot is opt-in for the joining client, re-simulating the
game stays the default and is always the fallback:

* the start script that connects the client decides per game, so a lobby can
  offer it as an option when rejoining:
  ```
  [game] { hostip=...; myplayername=...; mypasswd=...; snapshotjoin=1; }
  ```
* without that entry the `SnapshotJoin` config decides (default off).

The server offers snapshots unless `AllowSnapshotJoin` is off; every synced
client can be asked to provide one, regardless of its own `SnapshotJoin`.

## Rejoining after a crash

A player whose game crashed is still connected as far as the server is
concerned, until the connection timed out. Previously a new connection of
that player was rejected until the old one had been silent for
`ReconnectTimeout` (15 s, or the whole `NetworkTimeout` if the client gave
up before). Now a client that connects with the player's password replaces
the old connection at once (`RejoinReplacesConnection`, server, default on):
the newest connection of a player wins. Players without a password in the
start script still have to wait, as anyone could connect with their name.

## Joining client

1. `CPreGame` receives the snapshot before its player number, verifies and
   decompresses it and passes it to `CGame` as load handler.
2. `CGame` loads like a saved game (map, definitions and Lua are initialized
   normally, then the snapshot replaces the synced state) and starts playing
   without the start sequence (`CGame::StartPlayingFromSnapshot`).
3. The first game packet is the marker (`gs->frameNum` has to equal its
   frame); the client simulates the frames since then and plays live.
4. The history before the marker arrives in the background
   (`CSnapshotJoinClient::HandleHistory`). Its packets are not simulated, only
   recorded with the timing they would have had when received live
   (`CDemoRecorder::SaveHistoryToDemo`); the demo recorder inserts them right
   before the marker when the demo is written. Only effects outside of the
   synced state are applied (game id).

## Configuration

| key | side | default | |
|---|---|---|---|
| `SnapshotJoin` | client | `false` | ask for a snapshot when joining a running game (opt-in); the start script's `GAME\SnapshotJoin` overrides it per game |
| `AllowSnapshotJoin` | server | `true` | offer snapshots to joining clients |
| `RejoinReplacesConnection` | server | `true` | a player's new connection (correct password) replaces the old one at once |
| `SnapshotJoinSources` | server | `3` | clients asked (1 provider + verifiers) |
| `SnapshotJoinMinSources` | server | `1` | fewer suitable clients: catch up instead |
| `SnapshotJoinTimeout` | server | `60` | seconds without progress until falling back to catch-up |
| `SnapshotTransferRate` | both | 4 MiB/s | rate of snapshot uploads and joiner downloads |
| `SnapshotCompressionLevel` | source | `1` | LZMA2 level |

## Limitations

* Only the QTPFS path finder can serialize its state exactly; with the legacy
  path finder sources answer `UNSUPPORTED` and the joiner catches up.
* Native (C++) skirmish AIs keep their state only on their host. A client
  hosting one catches up instead. Lua AIs are synced gadgets and fully
  supported.
* Unsynced Lua (widgets, unsynced gadget parts) starts fresh on the joining
  client, like after `/luaui reload`.
* Snapshots are large: about 195 MB uncompressed in a 4-player game on a
  small map, most of it path finder and height map data. They are compressed
  with LZMA2 (about 15 MB, zlib gave 55 MB) in a background thread on the
  sources, which takes several seconds and about 450 MB of memory, and sent
  at up to `SnapshotTransferRate`, limited by the provider's upload and the
  server's bandwidth.
* A player without a password in the start script who restarted the game
  (e.g. after a crash) can only rejoin once the old connection was silent for
  `ReconnectTimeout`, like before; see "Rejoining after a crash".

## Testing

`test/snapshotjoin/` contains the tooling used to verify exactness:

* `run_integrity.py` - reference game vs. a game saved and loaded at a frame:
  per-frame sync checksums, byte comparison of the saved and the loaded state
  (`--compare-state`) and state dumps around a divergence (`--dump`);
  `--snapshot` saves and loads a game snapshot (`.srs`) instead of a saved game.
  Run it at an early *and* at a late, busy point of the game: a divergence can
  stay invisible to the sync checksums until the affected state is used (e.g.
  the synced RNG is drawn by every weapon but only matters once one fires).
  The config `SimStateTestDumpRNGFrames "first last"` logs every synced random
  number drawn in those frames, also right after a load
* `compare_states.py` - attributes differences between two states to the
  responsible class members
* `diagnose_first_frame.sh` - compares full states one frame after loading
* `run_mp_join.py` - end-to-end test: dedicated server, two players (one
  hosting two Lua AIs), a spectator; one player leaves (or is killed and
  restarts at once, `--kill`) and rejoins from a snapshot, opting in through
  the start script (also as spectator, `--bob-spectator`, while the game is
  paused, `--pause`, a second time in the same game, `--rejoin-twice`, with
  a provider only, `--sources 1`, or requiring verifier confirmation,
  `--sources 2 --min-sources 2`). Checks: the
  joiner simulated no frame before the snapshot and every frame after it
  matches the other clients; the snapshots of different sources are
  byte-identical and equal to what the joiner loaded (`--compare-snapshots`);
  the joiner's own demo replays the complete game like the original
  (`--check-demo`)
* `tools/gen_test_map.py` - deterministic test map, no downloads needed

## Trying it out (Windows)

The workflow `.github/workflows/snapshot-join-windows.yml` builds Windows and
Linux engine packages of this branch on regular GitHub runners (enable Actions
in the fork, then run "Snapshot join test build" or push to the branch). The
package (`recoil_<version>_amd64-windows-snapshotjoin.7z`) is in the run's
artifacts.

Nothing of a regular BAR installation is modified, if the test engine gets its
own folders:

1. Extract the package to e.g. `C:\recoil-snapshotjoin\engine` (not into the
   BAR installation).
2. Create an empty folder for everything the test engine writes, e.g.
   `C:\recoil-snapshotjoin\data`.
3. Start it with the installation's data folder (the one containing `games`,
   `maps`, `packages` and `pool`) as additional read-only content:

   ```bat
   set SPRING_DATADIR=%LOCALAPPDATA%\Programs\Beyond-All-Reason\data
   C:\recoil-snapshotjoin\engine\spring.exe --isolation --isolation-dir C:\recoil-snapshotjoin\data
   ```

   The first folder (`--isolation-dir`) receives all written files (config,
   cache, demos, infolog); `SPRING_DATADIR` is only read.

Every client of a game needs this engine. The game is hosted with
`spring-dedicated.exe` (or one of the clients) as usual. A client that
reconnects to the running game asks for a snapshot if its start script has
`snapshotjoin=1;` in the `[game]` section (or with `SnapshotJoin = 1` in its
`springsettings.cfg`). Its `infolog.txt`
shows `[SnapshotJoin]` lines, the host's log shows which clients provided and
verified it.

The automated multiplayer test also runs on Windows (Python 3), using the
installed game as read-only content and a generated test map:

```bat
python test\snapshotjoin\tools\gen_test_map.py C:\recoil-snapshotjoin\testdata\maps
python test\snapshotjoin\run_mp_join.py ^
  --headless C:\recoil-snapshotjoin\engine\spring-headless.exe ^
  --dedicated C:\recoil-snapshotjoin\engine\spring-dedicated.exe ^
  --data-dir C:\recoil-snapshotjoin\testdata ^
  --extra-data-dir %LOCALAPPDATA%\Programs\Beyond-All-Reason\data ^
  --game "<exact game archive name, e.g. Beyond All Reason test-12345-abcdef>" ^
  --work-dir C:\recoil-snapshotjoin\work --compare-snapshots --check-demo
```
