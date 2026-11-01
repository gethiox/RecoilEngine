#!/usr/bin/env python3
# This file is part of the Spring engine (GPL v2 or later), see LICENSE.html
"""
Simulation-state integrity harness for snapshot (save/load based) joining.

For a deterministic start script it runs up to three headless games:

  reference  plays frames [0, end]                                   -> ref.log
  saver      plays frames [0, end], saves right after frame <save>   -> save.log
  loader     loads that save and plays frames (save, end]            -> load.log

Each run records the engine's running sync checksum after every simulated
frame (see rts/Game/SimStateTestHooks.h). The harness then verifies that

  1. the reference run is reproducible (two reference runs are identical),
  2. saving does not perturb the saving client (saver == reference),
  3. the loaded game continues exactly like the uninterrupted one
     (loader == reference for every frame after <save>).

Any mismatch is reported with the first diverging frame; with --dump the
harness additionally asks the engine for DumpState() output around that
frame so the differing object/field can be located.

Exit code: 0 on success, 1 on any divergence, 2 on harness/engine errors.
"""

import argparse
import glob
import os
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))


def log(msg):
	print("[integrity] " + msg, flush=True)


def write_config(path, values):
	with open(path, "w") as f:
		for k, v in values.items():
			f.write("%s = %s\n" % (k, v))


def render_script(template, game, out_path):
	with open(template) as f:
		text = f.read()
	with open(out_path, "w") as f:
		f.write(text.replace("@GAME@", game))


def run_engine(args, name, start_arg, config):
	run_dir = os.path.join(args.work_dir, name)
	shutil.rmtree(run_dir, ignore_errors=True)
	os.makedirs(run_dir)

	cfg = dict(config)
	cfg.setdefault("SimStateTestDisableLuaUI", 1)
	cfg.setdefault("SimStateTestGameSpeed", args.speed)
	cfg_path = os.path.join(run_dir, "springsettings.cfg")
	write_config(cfg_path, cfg)

	cmd = [args.engine, "--isolation", "--isolation-dir", args.data_dir,
	       "--write-dir", run_dir, "--config", cfg_path, start_arg]
	log("%s: %s" % (name, " ".join(cmd)))
	t0 = time.time()
	with open(os.path.join(run_dir, "stdout.txt"), "w") as out:
		proc = subprocess.run(cmd, stdout=out, stderr=subprocess.STDOUT, cwd=run_dir, timeout=args.timeout)
	log("%s: exit code %d after %.1fs" % (name, proc.returncode, time.time() - t0))
	return run_dir, proc.returncode


def read_checksums(path):
	frames = {}
	if not os.path.exists(path):
		return frames
	with open(path) as f:
		for line in f:
			parts = line.split()
			if len(parts) == 2:
				frames[int(parts[0])] = parts[1]
	return frames


def compare(name_a, a, name_b, b, first, last):
	"""Return (ok, message); compares frames in [first, last]."""
	missing = [fr for fr in range(first, last + 1) if fr not in a or fr not in b]
	for fr in range(first, last + 1):
		if fr in a and fr in b and a[fr] != b[fr]:
			return False, "%s vs %s: first divergence at frame %d (%s != %s)" % (name_a, name_b, fr, a[fr], b[fr])
	if missing:
		return False, "%s vs %s: %d frames missing in [%d, %d] (first %d)" % (name_a, name_b, len(missing), first, last, missing[0])
	return True, "%s vs %s: frames %d..%d identical" % (name_a, name_b, first, last)


def main():
	ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
	ap.add_argument("--engine", required=True, help="path to spring-headless")
	ap.add_argument("--data-dir", required=True, help="isolation data dir holding games/, maps/ and engine base content")
	ap.add_argument("--game", required=True, help="full game name, e.g. 'Beyond All Reason $VERSION'")
	ap.add_argument("--script", default=os.path.join(HERE, "scripts", "ai-skirmish.txt"))
	ap.add_argument("--work-dir", required=True)
	ap.add_argument("--save-frame", type=int, required=True)
	ap.add_argument("--end-frame", type=int, required=True)
	ap.add_argument("--no-repro-check", action="store_true", help="skip the second reference run")
	ap.add_argument("--reuse-reference", action="store_true", help="reuse an existing reference run in work-dir")
	ap.add_argument("--dump", action="store_true", help="on divergence, rerun with DumpState around the diverging frame")
	ap.add_argument("--speed", type=float, default=1000.0, help="requested game speed (capped by the script's MaxSpeed)")
	ap.add_argument("--timeout", type=int, default=3600)
	args = ap.parse_args()

	args.engine = os.path.abspath(args.engine)
	args.data_dir = os.path.abspath(args.data_dir)
	args.work_dir = os.path.abspath(args.work_dir)

	if args.save_frame >= args.end_frame:
		ap.error("--save-frame must be smaller than --end-frame")

	os.makedirs(args.work_dir, exist_ok=True)
	script = os.path.join(args.work_dir, "script.txt")
	render_script(args.script, args.game, script)

	def checksum_cfg(run_name, extra=None):
		cfg = {"SimStateTestChecksumLog": os.path.join(args.work_dir, run_name + ".log"),
		       "SimStateTestQuitFrame": args.end_frame}
		cfg.update(extra or {})
		return cfg

	ref_log = os.path.join(args.work_dir, "ref.log")
	if not (args.reuse_reference and os.path.exists(ref_log)):
		_, rc = run_engine(args, "ref", script, checksum_cfg("ref"))
		if rc != 0:
			log("reference run failed")
			return 2
	ref = read_checksums(ref_log)
	if args.end_frame not in ref:
		log("reference run did not reach frame %d (last %s)" % (args.end_frame, max(ref) if ref else None))
		return 2

	results = []

	if not args.no_repro_check and not args.reuse_reference:
		run_engine(args, "ref2", script, checksum_cfg("ref2"))
		results.append(compare("ref", ref, "ref2", read_checksums(os.path.join(args.work_dir, "ref2.log")), 0, args.end_frame))

	save_dir, _ = run_engine(args, "save", script, checksum_cfg("save", {
		"SimStateTestSaveFrame": args.save_frame,
		"SimStateTestSaveName": "snap"}))
	results.append(compare("ref", ref, "save", read_checksums(os.path.join(args.work_dir, "save.log")), 0, args.end_frame))

	save_file = os.path.join(save_dir, "Saves", "snap.ssf")
	# the save is written asynchronously; the saver ran to completion so it must exist now
	if not os.path.exists(save_file):
		log("save file %s was not written" % save_file)
		return 2
	log("save size: %.1f MB" % (os.path.getsize(save_file) / 1e6))

	load_dir, _ = run_engine(args, "load", save_file, checksum_cfg("load"))
	load = read_checksums(os.path.join(args.work_dir, "load.log"))
	ok_load, msg_load = compare("ref", ref, "load", load, args.save_frame + 1, args.end_frame)
	results.append((ok_load, msg_load))

	if not ok_load and args.dump and load:
		# locate first divergent frame and ask both runs for a detailed state dump there
		div = next((fr for fr in range(args.save_frame + 1, args.end_frame + 1)
		            if fr in load and load[fr] != ref.get(fr)), args.save_frame + 1)
		lo, hi = args.save_frame, div
		dump_cfg = {"SimStateTestDumpFrames": "%d %d 1" % (lo, hi)}
		run_engine(args, "ref-dump", script, checksum_cfg("ref-dump", dump_cfg))
		run_engine(args, "load-dump", save_file, checksum_cfg("load-dump", dump_cfg))
		for name in ("ref-dump", "load-dump"):
			for p in glob.glob(os.path.join(args.work_dir, name, "*GameState-*.txt")):
				log("state dump: %s" % p)

	ok = True
	for (good, msg) in results:
		log(("PASS " if good else "FAIL ") + msg)
		ok &= good
	return 0 if ok else 1


if __name__ == "__main__":
	sys.exit(main())
