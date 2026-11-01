#!/usr/bin/env python3
# This file is part of the Spring engine (GPL v2 or later), see LICENSE.html
"""
Compare two game states written by SimStateTestCompareFrame
(compare-<frame>.state + compare-<frame>.layout) member by member.

The layout file lists, for every serialized class member, the byte range it
produced: "<start> <end> <class> <member>". Ranges nest (a member holding an
object contains that object's member ranges). Entries with class "<package>"
mark the independent creg packages (LuaGaia, LuaRules, Game); each package is
compared on its own so a size change in one does not hide differences in the
others. Bytes before the first package are the ECS components.

Differences are attributed to the innermost range covering them; the first
structural mismatch of a package (member sequence or size differs) is shown
with its enclosing members.

Usage: compare_states.py <stateA-basepath> <stateB-basepath> [--max N]
Exit code 0 if identical, 1 if they differ.
"""

import argparse
import collections
import sys


def load(base):
	with open(base + ".state", "rb") as f:
		data = f.read()
	entries = []
	packages = []
	with open(base + ".layout") as f:
		for line in f:
			parts = line.split(" ", 3)
			if len(parts) != 4:
				continue
			s, e, cls, mem = int(parts[0]), int(parts[1]), parts[2], parts[3].strip()
			if cls == "<package>":
				packages.append((mem, s, e))
			else:
				entries.append((s, e, cls + "::" + mem))
	return data, entries, packages


def package_entries(entries, start, end):
	return [(s - start, e - start, n) for (s, e, n) in entries if start <= s and e <= end]


def enclosing(entries, pos):
	chain = [e for e in entries if e[0] <= pos < e[1] or e[0] == pos == e[1]]
	return sorted(chain, key=lambda e: e[1] - e[0])


def is_leaf(entries, i):
	"""entries are in post-order: children directly precede their parent"""
	if i == 0:
		return True
	ps, pe, _ = entries[i - 1]
	s, e, _ = entries[i]
	return not (s <= ps and pe <= e and (pe > ps or s < e))


def compare_package(name, da, ea, db, eb, max_report):
	"""
	Walks both member sequences in parallel (they are produced in the same
	order) and compares leaf members by content, so size differences of one
	member do not misalign the rest. Stops at the first point where the
	sequences of member names diverge.
	Returns the number of differing member kinds (+1 for a structural break).
	"""
	if da == db:
		print("  [%s] identical (%d bytes)" % (name, len(da)))
		return 0

	diffs = collections.OrderedDict()
	structural = None

	for i, (x, y) in enumerate(zip(ea, eb)):
		if x[2] != y[2]:
			structural = i
			break
		if not is_leaf(ea, i):
			continue
		if da[x[0]:x[1]] != db[y[0]:y[1]]:
			diffs.setdefault(x[2], []).append(x[0])

	if structural is None and len(ea) != len(eb):
		structural = min(len(ea), len(eb))

	if not diffs and structural is None:
		print("  [%s] DIFFERENT outside of recorded members" % name)
		return 1

	print("  [%s] DIFFERENT: %d differing member kinds" % (name, len(diffs)))
	for i, (n, offsets) in enumerate(diffs.items()):
		if i >= max_report:
			print("    ...")
			break
		print("    %-70s x%-6d first at +%d" % (n, len(offsets), offsets[0]))

	if structural is not None:
		x = ea[structural] if structural < len(ea) else None
		y = eb[structural] if structural < len(eb) else None
		print("    MEMBER SEQUENCE differs at entry %d:  A=%s  B=%s" % (structural, x, y))
		if x is not None:
			for (cs, ce, cn) in enclosing(ea, x[0])[:10]:
				print("      in %-60s [+%d, +%d)" % (cn, cs, ce))

	return len(diffs) + (1 if structural is not None else 0)


def main():
	ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
	ap.add_argument("a")
	ap.add_argument("b")
	ap.add_argument("--max", type=int, default=40, help="number of differing members to list per package")
	args = ap.parse_args()

	data_a, ent_a, pkg_a = load(args.a)
	data_b, ent_b, pkg_b = load(args.b)

	print("sizes: %d vs %d bytes" % (len(data_a), len(data_b)))

	if data_a == data_b:
		print("IDENTICAL")
		return 0

	total = 0
	names_a = [p[0] for p in pkg_a]
	names_b = [p[0] for p in pkg_b]
	if names_a != names_b:
		print("package lists differ: %s vs %s" % (names_a, names_b))
		return 1

	# ECS components and other data before the first package
	pre_a = data_a[:pkg_a[0][1]] if pkg_a else data_a
	pre_b = data_b[:pkg_b[0][1]] if pkg_b else data_b
	if pre_a != pre_b:
		print("  [ECS components] DIFFERENT")
		total += 1
	else:
		print("  [ECS components] identical (%d bytes)" % len(pre_a))

	for (na, sa, ea_), (nb, sb, eb_) in zip(pkg_a, pkg_b):
		total += compare_package(na, data_a[sa:ea_], package_entries(ent_a, sa, ea_),
		                         data_b[sb:eb_], package_entries(ent_b, sb, eb_), args.max)

	print("DIFFERENT" if total else "IDENTICAL (modulo package framing)")
	return 1 if total else 0


if __name__ == "__main__":
	sys.exit(main())
