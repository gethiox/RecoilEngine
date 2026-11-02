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
* Sources serialize synchronously (the only stall), then compress and hash in
  a background thread and upload in the background.
* All bulk transfers (snapshot upload and relay, catch-up, history) are flow
  controlled: at most 32 kB are queued or unacknowledged on a link, so regular
  game packets are never stuck behind bulk data, and the reliable UDP protocol
  stays within its resend window (256 chunks).
* The server only reports success after every verifier's digest equals the
  provider's. The joiner checks the digest again after decompressing.
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
  data, members marked `CR_MEMBER_UNSYNCED` (e.g. draw positions);
* unsynced objects (visual-effect projectiles, lights, the local selection)
  are left out, including every reference to them, and get sync-ids from a
  separate range so synced objects get identical ids everywhere;
* the synced Lua heap is brought into a canonical form that does not change
  its behaviour (`creg::MakeCanonicalLuaState`): leaked values on idle
  thread stacks dropped, sizes that depend on the collector's (wall-clock
  driven) timing normalized, string table buckets sorted;
* per-query stamps (`tempNum`) are written canonically.

Loading keeps the joining client's identity and unsynced settings. Nothing is
recomputed on load: everything the simulation depends on is stored exactly,
including derived data that is updated lazily or paced over frames (height
map derivatives, LOS, path finder, smooth height mesh) and the storage order
of every container that is iterated (hash maps, Lua tables, ECS storages);
see the "Savegame:" commits.

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
| `SnapshotJoin` | client | `true` | ask for a snapshot when joining a running game |
| `AllowSnapshotJoin` | server | `true` | offer snapshots to joining clients |
| `SnapshotJoinSources` | server | `3` | clients asked (1 provider + verifiers) |
| `SnapshotJoinMinSources` | server | `1` | fewer suitable clients: catch up instead |
| `SnapshotJoinTimeout` | server | `60` | seconds without progress until falling back to catch-up |
| `SnapshotTransferRate` | both | 4 MiB/s | rate of snapshot uploads and joiner downloads |
| `SnapshotCompressionLevel` | source | `1` | zlib level |

## Limitations

* Only the QTPFS path finder can serialize its state exactly; with the legacy
  path finder sources answer `UNSUPPORTED` and the joiner catches up.
* Native (C++) skirmish AIs keep their state only on their host. A client
  hosting one catches up instead. Lua AIs are synced gadgets and fully
  supported.
* Unsynced Lua (widgets, unsynced gadget parts) starts fresh on the joining
  client, like after `/luaui reload`.
* Snapshots are large (about 55 MB compressed in a 4-player game on a small
  map, most of it path finder and height map data); they are sent at up to
  `SnapshotTransferRate`, limited by the provider's upload and the server's
  bandwidth.
* A client that crashed can only rejoin once the server noticed that its old
  connection is gone (`NetworkTimeout`), like before.

## Testing

`test/snapshotjoin/` contains the tooling used to verify exactness:

* `run_integrity.py` - reference game vs. a game saved and loaded at a frame:
  per-frame sync checksums, byte comparison of the saved and the loaded state
  (`--compare-state`) and state dumps around a divergence (`--dump`);
  `--snapshot` saves and loads a game snapshot (`.srs`) instead of a saved game
* `compare_states.py` - attributes differences between two states to the
  responsible class members
* `diagnose_first_frame.sh` - compares full states one frame after loading
* `run_mp_join.py` - end-to-end test: dedicated server, two players (one
  hosting two Lua AIs), a spectator; one player leaves (or is killed,
  `--kill`) and rejoins from a snapshot (also as spectator,
  `--bob-spectator`, or while the game is paused, `--pause`). Checks: the
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
`spring-dedicated.exe` (or one of the clients) as usual; a client that
reconnects to the running game asks for a snapshot. Its `infolog.txt`
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
