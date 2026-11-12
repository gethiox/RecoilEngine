#!/usr/bin/env python3
# This file is part of the Spring engine (GPL v2 or later), see LICENSE.html
"""
End-to-end snapshot-join test with a real multiplayer setup on localhost.

  server   spring-dedicated hosting the game
  alice    player  (also hosts two Lua AIs)
  bob      player  - leaves at --leave-frame and rejoins as a fresh process
  carol    spectator

The rejoining bob asks for a snapshot; the server picks carol (spectators are
preferred) as provider and alice as verifier. Checks:

  1. bob joined from a snapshot (log) and simulated no frame before it
  2. every frame bob simulated after joining has the same sync checksum as
     alice's and carol's (and the server reported no desync)
  3. optionally (--check-demo): bob's own demo of the second session is a
     complete replay; it is played back headless and must reach the end
     frame without desync, matching the reference checksums

Exit code 0 on success.
"""

import argparse
import glob
import os
import re
import shutil
import signal
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
PORT = 18452


def log(msg):
	print("[mp-join] " + msg, flush=True)


SERVER_SCRIPT = """[game]
{
	[allyteam0] { numallies=0; }
	[allyteam1] { numallies=0; }
	[team0] { teamleader=0; allyteam=0; side=Armada; }
	[team1] { teamleader=1; allyteam=1; side=Cortex; }
	[team2] { teamleader=0; allyteam=0; side=Cortex; }
	[team3] { teamleader=0; allyteam=1; side=Armada; }
	[player0] { name=Alice; team=0; spectator=0; password=pa; }
	[player1] { name=Bob; team=1; spectator=0; password=pb; }
	[player2] { name=Carol; team=0; spectator=1; password=pc; }
	[ai0] { team=2; host=0; shortname=SimpleAI; name=AI0; }
	[ai1] { team=3; host=0; shortname=SimpleAI; name=AI1; }
	[modoptions] { maxspeed=1000; }
	gametype=@GAME@;
	mapname=SnapshotTestMap;
	startpostype=0;
	fixedrngseed=123123;
	ishost=1;
	hostip=127.0.0.1;
	hostport=@PORT@;
	numplayers=3;
	numusers=5;
	autohostport=0;
}
"""

CLIENT_SCRIPT = """[game]
{
	hostip=127.0.0.1;
	hostport=@PORT@;
	ishost=0;
	myplayername=@NAME@;
	mypasswd=@PASS@;
	@EXTRA@
}
"""


class Proc:
	def __init__(self, name, cmd, cwd):
		self.name = name
		self.out = open(os.path.join(cwd, "stdout.txt"), "w")
		self.p = subprocess.Popen(cmd, cwd=cwd, stdout=self.out, stderr=subprocess.STDOUT)

	def alive(self):
		return self.p.poll() is None

	def stop(self, timeout=20):
		if self.alive():
			self.p.send_signal(signal.SIGTERM)
			try:
				self.p.wait(timeout)
			except subprocess.TimeoutExpired:
				self.p.kill()
		self.out.close()


def read_checksums(path):
	d = {}
	if os.path.exists(path):
		with open(path) as f:
			for line in f:
				parts = line.split()
				# a process killed while writing leaves a partial last line
				if len(parts) == 2 and len(parts[1]) == 8:
					d[int(parts[0])] = parts[1]
	return d


def last_frame(path):
	d = read_checksums(path)
	return max(d) if d else -1


def wait_for(cond, timeout, what):
	t0 = time.time()
	while not cond():
		if time.time() - t0 > timeout:
			raise RuntimeError("timeout waiting for " + what)
		time.sleep(0.5)


def check_demo(args, bob_dir, ref, engine_cmd, mkdir, write):
	"""plays bob's demo back headless; every frame has to match alice's checksums"""
	demos = sorted(glob.glob(os.path.join(bob_dir, "demos", "*.sdfz")))
	if not demos:
		log("FAIL bob wrote no demo")
		return False

	d = mkdir("replay")
	if args.cache_from and os.path.isdir(args.cache_from):
		shutil.copytree(args.cache_from, os.path.join(d, "cache"), dirs_exist_ok=True)

	cfg = {"SimStateTestChecksumLog": os.path.join(d, "checksums.log"), "SimStateTestQuitFrame": args.end_frame,
	       "SimStateTestGameSpeed": 1000, "LogFlushLevel": 0}
	write(os.path.join(d, "springsettings.cfg"), "".join("%s = %s\n" % kv for kv in cfg.items()))

	log("replaying " + os.path.basename(demos[-1]))
	t0 = time.time()
	with open(os.path.join(d, "stdout.txt"), "w") as out:
		try:
			subprocess.run(engine_cmd(args.headless, d, demos[-1]), cwd=d, stdout=out, stderr=subprocess.STDOUT, timeout=args.timeout)
		except subprocess.TimeoutExpired:
			log("FAIL demo playback timed out")
			return False

	rep = read_checksums(os.path.join(d, "checksums.log"))
	log("demo playback took %.1fs, frames %s..%s" % (time.time() - t0, min(rep) if rep else None, max(rep) if rep else None))

	common = sorted(set(rep) & set(ref))
	bad = [fr for fr in common if rep[fr] != ref[fr]]
	# the replay has to cover the whole game, including everything before the join
	complete = bool(rep) and min(rep) <= 1 and max(rep) >= args.end_frame - 1

	if bad or not complete:
		log("FAIL bob's demo: %d of %d frames differ from alice (first %s), %s" % (len(bad), len(common), bad[0] if bad else None, "complete" if complete else "incomplete"))
		return False

	log("PASS bob's demo replays frames %d..%d identically to alice" % (common[0], common[-1]))
	return True


def main():
	ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
	ap.add_argument("--headless", required=True)
	ap.add_argument("--dedicated", required=True)
	ap.add_argument("--data-dir", required=True, help="writable data directory (isolation dir): test map, server output")
	ap.add_argument("--extra-data-dir", help="read-only data directory with the game, e.g. of a regular BAR install (passed as SPRING_DATADIR)")
	ap.add_argument("--game", required=True)
	ap.add_argument("--work-dir", required=True)
	ap.add_argument("--leave-frame", type=int, default=1200)
	ap.add_argument("--end-frame", type=int, default=6000)
	ap.add_argument("--rejoin-delay", type=int, default=150, help="frames the game runs without bob")
	ap.add_argument("--speed", type=float, default=4, help="game speed requested by alice")
	ap.add_argument("--sources", type=int, default=3)
	ap.add_argument("--min-sources", type=int, default=None,
	                help="server's SnapshotJoinMinSources (default: the engine's 2, a verifier has to confirm; 1 with --bob-spectator, "
	                     "a spectator only gets spectator sources and this lobby has one)")
	ap.add_argument("--cooldown", type=int, default=None,
	                help="server's SnapshotJoinCooldown in seconds (default: the engine's 60; 0 with --rejoin-twice, so that the second "
	                     "rejoin uses a snapshot as well)")
	ap.add_argument("--rejoin-twice", type=int, default=0,
	                help="frames after the first rejoin at which bob leaves again and rejoins a second time (both from a snapshot)")
	ap.add_argument("--bob-spectator", action="store_true", help="bob is a spectator (only spectator sources allowed)")
	ap.add_argument("--no-snapshot", action="store_true", help="rejoin with classic catch-up (baseline)")
	ap.add_argument("--kill", action="store_true", help="kill bob (SIGKILL) instead of letting him quit; he rejoins before the server noticed")
	ap.add_argument("--timeout", type=int, default=1800)
	ap.add_argument("--compare-frame", type=int, default=-1,
		help="alice and carol write their snapshot data at this frame; compared with compare_states.py")
	ap.add_argument("--cache-from", help="engine 'cache' directory with an up to date archive cache to copy")
	ap.add_argument("--check-demo", action="store_true", help="replay bob's demo of his second session and compare it with alice")
	ap.add_argument("--pause", type=float, default=0, help="alice pauses the game for this many seconds when bob rejoins")
	ap.add_argument("--no-luaui", action="store_true", help="clients do not load LuaUI (saves memory)")
	ap.add_argument("--dump-frames", help="\"first last period\": bob (after rejoining) and carol write DumpState output for these frames")
	ap.add_argument("--compare-snapshots", action="store_true", help="alice and carol also write the join snapshots they create; they must be identical")
	args = ap.parse_args()

	if args.extra_data_dir:
		# only read from; every engine process writes into its own directory
		os.environ["SPRING_DATADIR"] = os.path.abspath(args.extra_data_dir)

	if args.cache_from:
		args.cache_from = os.path.abspath(args.cache_from)
		# the dedicated server writes into the isolation dir
		shutil.copytree(args.cache_from, os.path.join(os.path.abspath(args.data_dir), "cache"), dirs_exist_ok=True)

	for a in ("headless", "dedicated", "data_dir", "work_dir"):
		setattr(args, a, os.path.abspath(getattr(args, a)))

	shutil.rmtree(args.work_dir, ignore_errors=True)
	os.makedirs(args.work_dir)

	def mkdir(name):
		d = os.path.join(args.work_dir, name)
		os.makedirs(d, exist_ok=True)
		return d

	def write(path, text):
		with open(path, "w") as f:
			f.write(text)

	def engine_cmd(binary, d, script):
		cmd = [binary, "--isolation", "--isolation-dir", args.data_dir]
		# the dedicated server has no separate write-dir
		if binary != args.dedicated:
			cmd += ["--write-dir", d]
		return cmd + ["--config", os.path.join(d, "springsettings.cfg"), script]

	def seed_cache(d):
		# hashing the game archive takes minutes; reuse an existing archive cache
		if args.cache_from and os.path.isdir(args.cache_from):
			shutil.copytree(args.cache_from, os.path.join(d, "cache"), dirs_exist_ok=True)

	server_script = SERVER_SCRIPT.replace("@GAME@", args.game).replace("@PORT@", str(PORT))
	if args.bob_spectator:
		server_script = server_script.replace("[player1] { name=Bob; team=1; spectator=0;", "[player1] { name=Bob; team=1; spectator=1;")
		server_script = server_script.replace("[team1] { teamleader=1;", "[team1] { teamleader=0;")

	# the engine's defaults, except where this lobby cannot satisfy them
	if args.min_sources is None:
		args.min_sources = 1 if args.bob_spectator else 2
	if args.cooldown is None:
		args.cooldown = 0 if args.rejoin_twice else 60
	log("server: SnapshotJoinSources=%d SnapshotJoinMinSources=%d SnapshotJoinCooldown=%d" % (args.sources, args.min_sources, args.cooldown))

	procs = []
	try:
		sd = mkdir("server")
		write(os.path.join(sd, "script.txt"), server_script)
		write(os.path.join(sd, "springsettings.cfg"),
		      "AllowSnapshotJoin = %d\nSnapshotJoinSources = %d\nSnapshotJoinMinSources = %d\nSnapshotJoinCooldown = %d\nServerRecordDemos = 1\nLogFlushLevel = 0\n"
		      % (0 if args.no_snapshot else 1, args.sources, args.min_sources, args.cooldown))
		server = Proc("server", engine_cmd(args.dedicated, sd, os.path.join(sd, "script.txt")), sd)
		procs.append(server)
		time.sleep(2)

		def start_client(name, passwd, dirname, extra, script_extra=""):
			d = mkdir(dirname)
			seed_cache(d)
			write(os.path.join(d, "script.txt"),
			      CLIENT_SCRIPT.replace("@PORT@", str(PORT)).replace("@NAME@", name).replace("@PASS@", passwd).replace("@EXTRA@", script_extra))
			# joining from a snapshot is opt-in (config SnapshotJoin, default off)
			cfg = {"SimStateTestChecksumLog": os.path.join(d, "checksums.log"), "LogFlushLevel": 0}
			if args.no_luaui:
				cfg["SimStateTestDisableLuaUI"] = 1
			cfg.update(extra)
			write(os.path.join(d, "springsettings.cfg"), "".join("%s = %s\n" % kv for kv in cfg.items()))
			p = Proc(dirname, engine_cmd(args.headless, d, os.path.join(d, "script.txt")), d)
			procs.append(p)
			return p, d

		cmp = {"SimStateTestCompareFrame": args.compare_frame, "SimStateTestCompareSnapshot": 1} if args.compare_frame >= 0 else {}
		if args.compare_snapshots:
			cmp["SimStateTestDumpJoinSnapshots"] = 1
		dump = {"SimStateTestDumpFrames": args.dump_frames} if args.dump_frames else {}
		# alice only takes part in snapshot comparisons (state dumps need a lot of memory)
		alice_cmp = {k: v for (k, v) in cmp.items() if k == "SimStateTestDumpJoinSnapshots"}
		# players may pause the game to let someone rejoin
		rejoin_frame = args.leave_frame + args.rejoin_delay
		pause = {"SimStateTestPauseFrame": rejoin_frame, "SimStateTestPauseSeconds": args.pause} if args.pause > 0 else {}
		alice, alice_dir = start_client("Alice", "pa", "alice", dict(alice_cmp, SimStateTestGameSpeed=args.speed, SimStateTestQuitFrame=args.end_frame, **pause))
		carol, carol_dir = start_client("Carol", "pc", "carol", dict(cmp, SimStateTestQuitFrame=args.end_frame, **dump))
		bob1, bob1_dir = start_client("Bob", "pb", "bob1", {} if args.kill else {"SimStateTestQuitFrame": args.leave_frame})

		log("waiting for bob to reach frame %d" % args.leave_frame)
		wait_for(lambda: last_frame(os.path.join(bob1_dir, "checksums.log")) >= args.leave_frame, args.timeout, "bob leave frame")

		if args.kill:
			bob1.p.kill()
		wait_for(lambda: not bob1.alive(), 60, "bob exit")
		log("bob left at frame %d (alice at %d)" % (last_frame(os.path.join(bob1_dir, "checksums.log")), last_frame(os.path.join(alice_dir, "checksums.log"))))

		# let the game progress a bit without bob
		wait_for(lambda: last_frame(os.path.join(alice_dir, "checksums.log")) >= rejoin_frame or not alice.alive(), args.timeout, "alice rejoin frame")

		if not alice.alive():
			raise RuntimeError("the game ended before bob could rejoin, use a lower --speed or later --end-frame")

		t0 = time.time()
		# the lobby opts in for this join through the start script; a killed bob
		# rejoins at once, his new connection replaces the one the server still has
		leave2 = rejoin_frame + args.rejoin_twice
		bob2, bob2_dir = start_client("Bob", "pb", "bob2", dict(cmp, SimStateTestQuitFrame=(leave2 if args.rejoin_twice else args.end_frame), **dump),
		                              "" if args.no_snapshot else "snapshotjoin=1;")
		log("bob rejoining at alice frame %d" % last_frame(os.path.join(alice_dir, "checksums.log")))
		sessions = [("bob2", bob2_dir)]

		if args.rejoin_twice:
			# a second leave and rejoin: the sources create a second snapshot, the server runs a second join
			wait_for(lambda: not bob2.alive(), args.timeout, "bob second leave")
			log("bob left again at frame %d" % last_frame(os.path.join(bob2_dir, "checksums.log")))
			wait_for(lambda: last_frame(os.path.join(alice_dir, "checksums.log")) >= leave2 + args.rejoin_delay or not alice.alive(), args.timeout, "alice second rejoin frame")
			if not alice.alive():
				raise RuntimeError("the game ended before bob could rejoin a second time")
			bob3, bob3_dir = start_client("Bob", "pb", "bob3", dict(cmp, SimStateTestQuitFrame=args.end_frame, **dump),
			                              "" if args.no_snapshot else "snapshotjoin=1;")
			log("bob rejoining a second time at alice frame %d" % last_frame(os.path.join(alice_dir, "checksums.log")))
			sessions.append(("bob3", bob3_dir))
			bob2 = bob3

		for p in (alice, carol, bob2):
			wait_for(lambda p=p: not p.alive(), args.timeout, p.name + " end")

		log("bob's second session took %.1fs" % (time.time() - t0))
	finally:
		for p in procs:
			p.stop()

	# --- evaluation
	ok = True
	ref = read_checksums(os.path.join(alice_dir, "checksums.log"))
	car = read_checksums(os.path.join(carol_dir, "checksums.log"))

	# the dedicated server logs into the isolation (data) dir
	with open(os.path.join(args.data_dir, "infolog.txt"), errors="replace") as f:
		server_out = f.read()
	shutil.copy(os.path.join(args.data_dir, "infolog.txt"), os.path.join(sd, "infolog.txt"))

	for line in server_out.splitlines():
		if "[SnapshotJoin]" in line or "Sync error" in line or "sync" in line.lower() and "error" in line.lower():
			log("server: " + line.split("] ", 1)[-1][:160])

	if args.kill:
		# the server still had bob's old connection when he came back
		replaced = "Bob left the game:  terminating existing connection" in server_out
		log(("PASS" if replaced else "FAIL") + " bob's new connection replaced the one the server still had")
		ok &= replaced

	for session, bob_dir in sessions:
		bob = read_checksums(os.path.join(bob_dir, "checksums.log"))

		with open(os.path.join(bob_dir, "stdout.txt"), errors="replace") as f:
			bob_out = f.read()

		for line in bob_out.splitlines():
			if "[SnapshotJoin]" in line:
				log(session + ": " + line.split("] ", 1)[-1][:160])

		joined = re.search(r"continuing the game from the snapshot of frame (\d+)", bob_out)
		if not args.no_snapshot:
			log(("PASS" if joined else "FAIL") + " %s joined from a snapshot" % session)
			ok &= (joined is not None)
			history = re.search(r"received the game history", bob_out) is not None
			log(("PASS" if history else "FAIL") + " %s received the game history" % session)
			ok &= history

		if not bob:
			log("FAIL %s simulated no frames" % session)
			return 1

		first = min(bob)
		log("%s simulated frames %d..%d" % (session, first, max(bob)))

		if joined is not None and not args.no_snapshot:
			marker = int(joined.group(1))
			if args.pause > 0 and session == "bob2":
				# the snapshot was taken while the game was paused (the pause takes effect a few frames later)
				paused = (rejoin_frame <= marker <= rejoin_frame + 2 * 30)
				log(("PASS" if paused else "FAIL") + " bob joined the game paused after frame %d (snapshot frame %d)" % (rejoin_frame, marker))
				ok &= paused
			# the snapshot covers every frame up to its marker
			good = (first == marker + 1)
			log(("PASS" if good else "FAIL") + " %s's first simulated frame %d follows the snapshot frame %d" % (session, first, marker))
			ok &= good

		for name, other in (("alice", ref), ("carol", car)):
			common = sorted(set(bob) & set(other))
			bad = [fr for fr in common if bob[fr] != other[fr]]
			if bad or not common:
				log("FAIL %s vs %s: %d of %d common frames differ (first %s)" % (session, name, len(bad), len(common), bad[0] if bad else None))
				ok = False
			else:
				log("PASS %s vs %s: %d common frames identical (%d..%d)" % (session, name, len(common), common[0], common[-1]))

	# the last session is the one checked below (demo, loaded states)
	bob2_dir = sessions[-1][1]

	def compare_state(name_a, path_a, name_b, path_b, what):
		if not (os.path.exists(path_a + ".state") and os.path.exists(path_b + ".state")):
			log("SKIP %s: %s or %s missing" % (what, name_a, name_b))
			return True
		rc = subprocess.run([sys.executable, os.path.join(HERE, "compare_states.py"), path_a, path_b]).returncode
		log(("PASS" if rc == 0 else "FAIL") + " %s of %s and %s %s" % (what, name_a, name_b, "identical" if rc == 0 else "differ"))
		return rc == 0

	if args.compare_frame >= 0:
		base = "compare-%d" % args.compare_frame
		ok &= compare_state("bob", os.path.join(bob2_dir, base), "carol", os.path.join(carol_dir, base), "snapshot data at frame %d" % args.compare_frame)

	if args.compare_snapshots:
		frames = lambda d: {os.path.basename(f)[len("snapshot-"):-len(".state")] for f in glob.glob(os.path.join(d, "snapshot-*.state"))}
		# a joining spectator only gets spectator sources, so alice writes no snapshot then
		common = sorted(frames(carol_dir) & (frames(carol_dir) if args.bob_spectator else frames(alice_dir)), key=int)
		if not common:
			log("FAIL no join snapshot written by %s" % ("carol" if args.bob_spectator else "both alice and carol"))
			ok = False
		for fr in common:
			base = "snapshot-" + fr
			if not args.bob_spectator:
				ok &= compare_state("alice", os.path.join(alice_dir, base), "carol", os.path.join(carol_dir, base), "join snapshots of frame " + fr)
			# what bob loaded (in whichever of his sessions)
			loaded = [d for (_, d) in sessions if os.path.exists(os.path.join(d, "loaded-" + fr + ".state"))]
			ok &= compare_state("carol", os.path.join(carol_dir, base), "bob", os.path.join(loaded[0] if loaded else bob2_dir, "loaded-" + fr), "join snapshot and loaded state of frame " + fr)

	if args.check_demo:
		ok &= check_demo(args, bob2_dir, ref, engine_cmd, mkdir, write)

	if "Sync error" in server_out:
		log("FAIL server reported a sync error")
		ok = False

	return 0 if ok else 1


if __name__ == "__main__":
	sys.exit(main())
