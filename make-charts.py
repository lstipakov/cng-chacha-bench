#!/usr/bin/env python3
"""Regenerate the README charts from the measured numbers. No dependencies.

    python make-charts.py
"""

import os

# Gbit/s, as reported by bench.exe. See README.md.
CPUS = [
    ("Core i5-1335U",        "AVX2",    {1400: (43.3, 3.9, 15.0), 8192: (76.8, 4.0, 20.4), 65536: (82.8, 4.1, 20.5)}),
    ("EPYC 7763",            "AVX2",    {1400: (26.8, 2.7, 12.3), 8192: (42.0, 2.9, 16.9), 65536: (45.3, 2.9, 17.6)}),
    ("Xeon Platinum 8573C",  "AVX-512", {1400: (39.9, 3.4, 15.8), 8192: (58.9, 3.6, 26.6), 65536: (64.1, 3.6, 28.7)}),
]

THEMES = {
    "light": dict(fg="#1f2328", dim="#656d76", grid="#d1d9e0", bg="none"),
    "dark":  dict(fg="#e6edf3", dim="#9198a1", grid="#3d444d", bg="none"),
}
AES, CNG, CGS = "#2f6fd0", "#d1242f", "#1a7f37"


def esc(s):
    return s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")


def svg_open(w, h, t):
    return [
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{w}" height="{h}" '
        f'viewBox="0 0 {w} {h}" font-family="-apple-system,Segoe UI,Helvetica,Arial,sans-serif">',
        f'<rect width="{w}" height="{h}" fill="{t["bg"]}"/>',
    ]


def legend(out, x, y, t, items):
    for label, colour in items:
        out.append(f'<rect x="{x}" y="{y - 9}" width="11" height="11" rx="2" fill="{colour}"/>')
        out.append(f'<text x="{x + 17}" y="{y}" font-size="12" fill="{t["fg"]}">{esc(label)}</text>')
        x += 24 + int(len(label) * 6.6)


def chart_throughput(theme):
    """One group per CPU, three bars each, at a 1400-byte payload."""
    t = THEMES[theme]
    left, right, top = 186, 54, 54
    bar_h, gap, group_gap = 19, 5, 26
    rows = len(CPUS) * 3
    plot_h = rows * bar_h + (rows - len(CPUS)) * gap + (len(CPUS) - 1) * group_gap
    w, h = 760, top + plot_h + 44
    plot_w = w - left - right
    vmax = 90.0

    out = svg_open(w, h, t)
    out.append(f'<text x="0" y="18" font-size="14" font-weight="600" fill="{t["fg"]}">'
               f'ChaCha20-Poly1305 and AES-256-GCM, 1400-byte payloads</text>')
    out.append(f'<text x="0" y="35" font-size="12" fill="{t["dim"]}">Gbit/s, one core. Longer is faster.</text>')
    legend(out, 0, h - 12, t, [("CNG ChaCha20-Poly1305", CNG),
                               ("CRYPTOGAMS ChaCha20-Poly1305", CGS),
                               ("CNG AES-256-GCM", AES)])

    for gx in range(0, int(vmax) + 1, 15):
        x = left + plot_w * gx / vmax
        out.append(f'<line x1="{x:.1f}" y1="{top - 6}" x2="{x:.1f}" y2="{top + plot_h}" '
                   f'stroke="{t["grid"]}" stroke-width="1"/>')
        out.append(f'<text x="{x:.1f}" y="{top - 12}" font-size="10" fill="{t["dim"]}" '
                   f'text-anchor="middle">{gx}</text>')

    y = top
    for name, iset, data in CPUS:
        aes, cng, cgs = data[1400]
        out.append(f'<text x="{left - 10}" y="{y + bar_h + 4}" font-size="12" font-weight="600" '
                   f'fill="{t["fg"]}" text-anchor="end">{esc(name)}</text>')
        out.append(f'<text x="{left - 10}" y="{y + bar_h + 19}" font-size="10" '
                   f'fill="{t["dim"]}" text-anchor="end">{esc(iset)}</text>')
        for value, colour in ((cng, CNG), (cgs, CGS), (aes, AES)):
            bw = plot_w * value / vmax
            out.append(f'<rect x="{left}" y="{y}" width="{bw:.1f}" height="{bar_h}" rx="2" fill="{colour}"/>')
            out.append(f'<text x="{left + bw + 7:.1f}" y="{y + bar_h - 5}" font-size="11" '
                       f'fill="{t["fg"]}">{value:.1f}</text>')
            y += bar_h + gap
        y += group_gap - gap

    out.append('</svg>')
    return "\n".join(out)


def chart_scaling(theme):
    """Payload size against throughput: AES amortises, ChaCha does not move."""
    t = THEMES[theme]
    sizes = [1400, 8192, 65536]
    labels = ["1 400 B", "8 192 B", "65 536 B"]
    left, right, top = 84, 150, 54
    bar_h, gap, group_gap = 21, 6, 24
    plot_h = len(sizes) * (bar_h * 2 + gap) + (len(sizes) - 1) * group_gap
    w, h = 760, top + plot_h + 44
    plot_w = w - left - right
    vmax = 70.0
    _, _, data = CPUS[2]

    out = svg_open(w, h, t)
    out.append(f'<text x="0" y="18" font-size="14" font-weight="600" fill="{t["fg"]}">'
               f'The same provider, the same core, three payload sizes</text>')
    out.append(f'<text x="0" y="35" font-size="12" fill="{t["dim"]}">'
               f'Xeon Platinum 8573C. Gbit/s. A 47x larger buffer leaves ChaCha where it started.</text>')
    legend(out, 0, h - 12, t, [("CNG ChaCha20-Poly1305", CNG), ("CNG AES-256-GCM", AES)])

    y = top
    for size, label in zip(sizes, labels):
        aes, cng, _ = data[size]
        out.append(f'<text x="{left - 10}" y="{y + bar_h + 6}" font-size="12" '
                   f'fill="{t["fg"]}" text-anchor="end">{esc(label)}</text>')
        for value, colour, name in ((cng, CNG, "ChaCha"), (aes, AES, "AES")):
            bw = plot_w * value / vmax
            out.append(f'<rect x="{left}" y="{y}" width="{bw:.1f}" height="{bar_h}" rx="2" fill="{colour}"/>')
            out.append(f'<text x="{left + bw + 7:.1f}" y="{y + bar_h - 6}" font-size="11" '
                       f'fill="{t["fg"]}">{value:.1f} <tspan fill="{t["dim"]}">{name}</tspan></text>')
            y += bar_h + gap
        y += group_gap - gap

    out.append('</svg>')
    return "\n".join(out)


os.makedirs("charts", exist_ok=True)
for theme in THEMES:
    for name, fn in (("throughput", chart_throughput), ("scaling", chart_scaling)):
        path = os.path.join("charts", f"{name}-{theme}.svg")
        with open(path, "w", encoding="utf-8", newline="\n") as fh:
            fh.write(fn(theme) + "\n")
        print("wrote", path)
