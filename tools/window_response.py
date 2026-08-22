#!/usr/bin/env python3
"""Plot the frequency response of the cosine-sum window shaders, as implemented.

One-off diagnostic for the Blackman-Harris fourth-term bug: BlackmanHarrisWindow.glsl
and ComplexBlackmanHarrisWindow.glsl evaluated the fourth term at cos(6x) where the
definition has cos(3x). Coherent gain is unchanged (both cosines average to zero over
a period), so every amplitude calibration check still passes, but the four-term
cancellation that gives Blackman-Harris its -92 dB sidelobes is destroyed.

The window is not hardcoded here. Each variant's coefficients and cosine harmonics are
parsed out of the GLSL source, so the plot shows what the shader actually computes:

    float w =
        alpha0 -
        alpha1 * cos(num) +
        alpha2 * cos(2*num) -
        alpha3 * cos(3*num);      // num = n * 2*pi/npoints

The phase step matches the callers (ComplexFFTFilter.cpp:402, FFTFilter.cpp:246), which
pass scale = 2*pi/npoints - i.e. the periodic (DFT-symmetric) form of the window.

Usage:
    tools/window_response.py                     # HEAD vs working tree, writes a PNG
    tools/window_response.py --check             # also fail if a shader's PSL > -90 dB
    tools/window_response.py --left reference    # compare against the textbook definition

A variant spec is a shader path, `git:<rev>:<path-in-repo>`, `reference` (the correct
Blackman-Harris definition) or `reported` (the cos(6x) form from the bug report).
"""

import argparse
import os
import re
import subprocess
import sys

import numpy as np

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SCOPEHAL = os.path.join(REPO, "lib", "scopehal")

COMPLEX_BH = "scopeprotocols/shaders/ComplexBlackmanHarrisWindow.glsl"
REAL_BH = "scopeprotocols/shaders/BlackmanHarrisWindow.glsl"
PACKED = os.path.join(REPO, "src/imcufosphor/shaders/PackedComplexWindow.glsl")

#(sign, coefficient, harmonic). Harmonic 0 is the DC term.
BH_REFERENCE = [(1, 0.35875, 0), (-1, 0.48829, 1), (1, 0.14128, 2), (-1, 0.01168, 3)]
BH_REPORTED = [(1, 0.35875, 0), (-1, 0.48829, 1), (1, 0.14128, 2), (-1, 0.01168, 6)]

#Categorical slots 2 and 1 of the reference palette, plus its light-mode ink and surface
COLOR_BROKEN = "#eb6834"
COLOR_FIXED = "#2a78d6"
INK = "#0b0b0b"
INK_MUTED = "#52514e"
SURFACE = "#fcfcfb"
GRID = "#e8e7e2"
AXIS = "#d5d4cf"


# ----------------------------------------------------------------------------------
# Parsing the window out of the shader
# ----------------------------------------------------------------------------------

def parse_window(text):
	"""Extracts the cosine-sum terms from a window shader's `float w = ...;` statement.

	Returns a list of (sign, coefficient, harmonic) tuples. Named coefficients are
	resolved against the shader's `const float` declarations; a term with no cos()
	is the DC term and gets harmonic 0.
	"""
	consts = {}
	for name, value in re.findall(r"const\s+float\s+(\w+)\s*=\s*([-+0-9.eE]+)\s*;", text):
		consts[name] = float(value)

	m = re.search(r"float\s+w\s*=(.*?);", text, re.S)
	if not m:
		raise ValueError("no `float w = ...;` statement found")

	#Strip comments and all whitespace so the term split below sees a flat expression
	expr = re.sub(r"//[^\n]*", "", m.group(1))
	expr = re.sub(r"\s+", "", expr)

	terms = []
	for tok in re.split(r"(?=[+-])", expr):
		if not tok:
			continue
		tm = re.match(r"^([+-]?)(\w+)(?:\*cos\((?:([0-9.]+)\*)?\w+\))?$", tok)
		if not tm:
			raise ValueError("unrecognized window term %r" % tok)

		sign = -1 if tm.group(1) == "-" else 1
		name = tm.group(2)
		try:
			coeff = float(name)
		except ValueError:
			#Unresolved names are push constants (PackedComplexWindow.glsl); the caller
			#substitutes the values it wants to measure
			coeff = consts.get(name)

		if "cos(" in tok:
			harmonic = float(tm.group(3)) if tm.group(3) else 1.0
		else:
			harmonic = 0.0

		terms.append((sign, coeff, harmonic))

	return terms


def describe(terms):
	"""One-line rendering of the parsed window, e.g. `0.35875 - 0.48829*cos(x) + ...`."""
	out = []
	for i, (sign, coeff, harmonic) in enumerate(terms):
		op = "-" if sign < 0 else "+"
		c = "a%d" % i if coeff is None else "%g" % coeff
		if harmonic == 0:
			body = c
		elif harmonic == 1:
			body = "%s*cos(x)" % c
		else:
			body = "%s*cos(%g x)" % (c, harmonic)
		out.append(body if i == 0 and sign > 0 else "%s %s" % (op, body))
	return " ".join(out)


def load_variant(spec):
	"""Resolves a variant spec to (terms, provenance)."""
	if spec == "reference":
		return BH_REFERENCE, "textbook Blackman-Harris definition"
	if spec == "reported":
		return BH_REPORTED, "cos(6x) fourth term, as described in the bug report"

	if spec.startswith("git:"):
		_, rev, path = spec.split(":", 2)
		repo = SCOPEHAL if not os.path.isabs(path) else REPO
		text = subprocess.check_output(
			["git", "-C", repo, "show", "%s:%s" % (rev, path)], text=True)
		return parse_window(text), "%s @ %s" % (path, rev)

	with open(spec) as f:
		text = f.read()
	root = SCOPEHAL if os.path.abspath(spec).startswith(SCOPEHAL) else REPO
	return parse_window(text), "%s (working tree)" % os.path.relpath(spec, root)


# ----------------------------------------------------------------------------------
# Window synthesis and metrics
# ----------------------------------------------------------------------------------

def synthesize(terms, npoints, dtype=np.float64):
	"""Evaluates the window exactly as the shader does, for n in [0, npoints)."""
	n = np.arange(npoints, dtype=dtype)
	phase_step = dtype(2.0 * np.pi / npoints)
	num = n * phase_step
	w = np.zeros(npoints, dtype=dtype)
	for sign, coeff, harmonic in terms:
		if harmonic == 0:
			w += dtype(sign * coeff)
		else:
			w += dtype(sign * coeff) * np.cos(dtype(harmonic) * num, dtype=dtype)
	return w


def response(w, oversample=64):
	"""Returns (frequency in DFT bins, magnitude in dB relative to the mainlobe peak)."""
	npoints = len(w)
	nfft = npoints * oversample
	mag = np.abs(np.fft.fft(w.astype(np.float64), nfft))
	db = 20 * np.log10(np.maximum(mag / mag.max(), 1e-300))
	freq = np.fft.fftfreq(nfft) * npoints
	return np.fft.fftshift(freq), np.fft.fftshift(db)


def peak_sidelobe(freq, db):
	"""Peak sidelobe level in dB and its frequency, searching outward from the first null."""
	pos = freq > 0
	f, d = freq[pos], db[pos]

	#First null: the first sample that is a local minimum well below the peak
	null = None
	for i in range(1, len(d) - 1):
		if d[i] < d[i - 1] and d[i] <= d[i + 1] and d[i] < -20:
			null = i
			break
	if null is None:
		return float("nan"), float("nan")

	j = null + int(np.argmax(d[null:]))
	return d[j], f[j]


def crossing(freq, db, level):
	"""Positive-frequency width at `level` dB, in bins (full width, both sides)."""
	pos = (freq >= 0) & (freq < 8)
	f, d = freq[pos], db[pos]
	below = np.nonzero(d < level)[0]
	if len(below) == 0:
		return float("nan")
	i = below[0]
	#Linear interpolation between the last sample above the level and the first below
	f0, f1, d0, d1 = f[i - 1], f[i], d[i - 1], d[i]
	return 2 * (f0 + (level - d0) * (f1 - f0) / (d1 - d0))


def metrics(w, freq, db):
	npoints = len(w)
	psl, psl_freq = peak_sidelobe(freq, db)
	half_bin = np.interp(0.5, freq[freq >= 0], db[freq >= 0])
	return {
		"coherent_gain": w.sum() / npoints,
		"psl_db": psl,
		"psl_bin": psl_freq,
		"enbw_bins": npoints * (w ** 2).sum() / (w.sum() ** 2),
		"bw_3db": crossing(freq, db, -3.0),
		"bw_6db": crossing(freq, db, -6.0),
		"scalloping_db": half_bin,
	}


# ----------------------------------------------------------------------------------
# Plot
# ----------------------------------------------------------------------------------

def style_axis(ax, ylim):
	ax.set_facecolor(SURFACE)
	ax.set_ylim(*ylim)
	ax.grid(True, axis="y", color=GRID, linewidth=0.8)
	ax.set_axisbelow(True)
	for side in ("top", "right"):
		ax.spines[side].set_visible(False)
	for side in ("left", "bottom"):
		ax.spines[side].set_color(AXIS)
		ax.spines[side].set_linewidth(1.0)
	ax.tick_params(colors=INK_MUTED, labelsize=9, length=4, width=1.0)


def plot(panels, out_path, span, ylim):
	import matplotlib
	matplotlib.use("Agg")
	import matplotlib.pyplot as plt

	fig, axes = plt.subplots(1, 2, figsize=(12.0, 5.6), sharey=True)
	fig.patch.set_facecolor(SURFACE)

	for ax, panel in zip(axes, panels):
		style_axis(ax, ylim)
		freq, db, m = panel["freq"], panel["db"], panel["metrics"]

		sel = np.abs(freq) <= span
		ax.plot(freq[sel], db[sel], color=panel["color"], linewidth=1.6,
			solid_capstyle="round", zorder=3)

		#Direct label on the one number the plot exists to compare
		ax.axhline(m["psl_db"], color=INK_MUTED, linewidth=1.0, linestyle=(0, (4, 3)),
			zorder=2)
		ax.annotate("peak sidelobe  %.1f dB" % m["psl_db"],
			xy=(-span * 0.96, m["psl_db"]), xytext=(0, 5), textcoords="offset points",
			ha="left", va="bottom", fontsize=10, color=INK, zorder=4,
			bbox=dict(boxstyle="round,pad=0.25", facecolor=SURFACE, edgecolor="none"))

		#Panel header: role, then the expression, then where it came from
		ax.text(0.0, 1.135, panel["title"], transform=ax.transAxes, ha="left",
			va="bottom", fontsize=12.5, color=INK, fontweight="medium")
		ax.text(0.0, 1.065, panel["formula"], transform=ax.transAxes, ha="left",
			va="bottom", fontsize=9.5, color=INK, family="monospace")
		ax.text(0.0, 1.015, panel["source"], transform=ax.transAxes, ha="left",
			va="bottom", fontsize=8.5, color=INK_MUTED)

		#Secondary numbers live inside the panel, in the empty upper corner
		ax.text(0.985, 0.965,
			"ENBW %.3f bins\ncoherent gain %.4f\n-3 dB width %.2f bins"
				% (m["enbw_bins"], m["coherent_gain"], m["bw_3db"]),
			transform=ax.transAxes, ha="right", va="top", fontsize=8.5,
			color=INK_MUTED, linespacing=1.5, zorder=4)

		ax.set_xlabel("frequency offset (DFT bins)", fontsize=9.5, color=INK_MUTED)
		ax.set_xlim(-span, span)

	axes[0].set_ylabel("magnitude (dB rel. peak)", fontsize=9.5, color=INK_MUTED)
	fig.tight_layout()
	fig.savefig(out_path, dpi=160, facecolor=SURFACE)
	return out_path


# ----------------------------------------------------------------------------------

def display(path):
	"""Repo-relative name for a shader, or its plain path if it lives outside the repo."""
	rel = os.path.relpath(path, REPO)
	return path if rel.startswith("..") else rel


def report(rows):
	head = ("%-68s %10s %10s %8s %8s %9s"
		% ("window", "PSL (dB)", "PSL (bin)", "ENBW", "CG", "scallop"))
	print(head)
	print("-" * len(head))
	for name, m in rows:
		print("%-68s %10.1f %10.2f %8.3f %8.4f %9.2f"
			% (name, m["psl_db"], m["psl_bin"], m["enbw_bins"], m["coherent_gain"],
				m["scalloping_db"]))


def main():
	ap = argparse.ArgumentParser(description=__doc__,
		formatter_class=argparse.RawDescriptionHelpFormatter)
	ap.add_argument("--left", default=None,
		help="left panel variant (default: %s at HEAD)" % COMPLEX_BH)
	ap.add_argument("--right", default=None,
		help="right panel variant (default: the working tree shader)")
	ap.add_argument("--npoints", type=int, default=4096, help="window length")
	ap.add_argument("--oversample", type=int, default=64, help="FFT zero-pad factor")
	ap.add_argument("--span", type=float, default=40.0, help="x range, +/- bins")
	ap.add_argument("--float32", action="store_true",
		help="synthesize in float32, as the shader does")
	ap.add_argument("--out", default=os.path.join(REPO, "build", "window_response.png"))
	ap.add_argument("--check", action="store_true",
		help="exit nonzero if any working-tree shader's peak sidelobe is above -90 dB")
	ap.add_argument("shaders", nargs="*",
		help="additional window shaders to measure and check")
	args = ap.parse_args()

	dtype = np.float32 if args.float32 else np.float64

	left_spec = args.left or "git:HEAD:" + COMPLEX_BH
	right_spec = args.right or os.path.join(SCOPEHAL, COMPLEX_BH)

	left_terms, left_source = load_variant(left_spec)
	right_terms, right_source = load_variant(right_spec)

	if left_terms == right_terms and args.left is None:
		print("note: %s is unchanged from HEAD; comparing against the reported cos(6x) "
			"form instead\n" % COMPLEX_BH, file=sys.stderr)
		left_terms, left_source = load_variant("reported")

	panels = []
	for title, terms, source, color in (
		("As implemented", left_terms, left_source, COLOR_BROKEN),
		("Corrected", right_terms, right_source, COLOR_FIXED),
	):
		w = synthesize(terms, args.npoints, dtype)
		freq, db = response(w, args.oversample)
		panels.append({
			"title": title,
			"formula": "w = " + describe(terms),
			"source": "",
			"color": color,
			"freq": freq,
			"db": db,
			"npoints": args.npoints,
			"metrics": metrics(w.astype(np.float64), freq, db),
		})

	#Every window shader in the tree gets measured, not just the two plotted, since
	#all three carry the same cosine-sum expression
	rows = [("%s panel: %s" % (side, p["source"]), p["metrics"])
		for side, p in zip(("left", "right"), panels)]
	failures = []
	sweep = [os.path.join(SCOPEHAL, REAL_BH), os.path.join(SCOPEHAL, COMPLEX_BH), PACKED]
	for path in sweep + [os.path.abspath(p) for p in args.shaders]:
		if not os.path.exists(path):
			continue
		with open(path) as f:
			text = f.read()
		try:
			terms = parse_window(text)
		except ValueError as e:
			print("skipping %s: %s" % (display(path), e), file=sys.stderr)
			continue
		#PackedComplexWindow's coefficients are push constants, so substitute
		#Blackman-Harris' values to measure the harmonics it actually evaluates
		if any(c is None for _, c, _ in terms):
			terms = [(s, BH_REFERENCE[i][1] if c is None else c, h)
				for i, (s, c, h) in enumerate(terms)]
		w = synthesize(terms, args.npoints, dtype)
		freq, db = response(w, args.oversample)
		m = metrics(w.astype(np.float64), freq, db)
		rows.append((display(path), m))
		if m["psl_db"] > -90.0:
			failures.append((display(path), m["psl_db"]))

	report(rows)

	out = plot(panels, args.out, args.span, (-140.0, 6.0))
	print("\nwrote %s" % out)

	if args.check and failures:
		print("\nFAIL: peak sidelobe above -90 dB in:", file=sys.stderr)
		for name, psl in failures:
			print("  %s: %.1f dB" % (name, psl), file=sys.stderr)
		return 1
	return 0


if __name__ == "__main__":
	sys.exit(main())
