#!/usr/bin/env python3
"""Generate cvm-counters.svg: a table figure in the same style as the other figures."""

FONT = "Helvetica, Arial, 'Liberation Sans', sans-serif"
MONO = "'Courier New', 'Liberation Mono', monospace"
BLUE = "#2e5c9e"
PEACH = "#fce4d6"
PANEL = "#eceef2"
INK = "#222222"
GRAY = "#595959"

rows = [
    ("RX total",                     "65,650,022", "—"),
    ("Accepted by CVM RX",           "—", "65,623,596"),
    ("RX missed (queue full)",       "—", "26,426"),
    ("TX requested",                 "—", "65,623,596"),
    ("TX accepted",                  "—", "65,608,679"),
    ("TX dropped (burst full)",      "—", "14,917"),
    ("Gb/s, 1514 B frame (testpmd)", "—", "98.3–98.4"),
]
cols = ["Counter", "Host physical port", "CVM testpmd"]
X = [40, 420, 660]          # column left edges
R = [None, 620, 860]        # right edges for right-aligned numeric columns
ROW_H, HEAD_H, TOP = 38, 44, 30
W, H = 900, TOP + HEAD_H + ROW_H * len(rows) + 30

out = []
def add(s): out.append(s)
def text(x, y, s, size=17, anchor="start", weight="normal", fill=INK, family=FONT):
    add(f'<text x="{x}" y="{y}" font-family="{family}" font-size="{size}" font-weight="{weight}" text-anchor="{anchor}" fill="{fill}">{s}</text>')
def hline(y, sw=1.5, color=INK):
    add(f'<line x1="{X[0]}" y1="{y}" x2="{R[2]}" y2="{y}" stroke="{color}" stroke-width="{sw}"/>')

add(f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" viewBox="0 0 {W} {H}">')
add(f'<rect width="{W}" height="{H}" fill="#fff"/>')

# header
y = TOP
add(f'<rect x="{X[0]}" y="{y}" width="{R[2]-X[0]}" height="{HEAD_H}" fill="{PANEL}"/>')
hline(y, 2)
text(X[0] + 12, y + 29, cols[0], weight="bold")
text(R[1], y + 29, cols[1], anchor="end", weight="bold")
text(R[2], y + 29, cols[2], anchor="end", weight="bold")
y += HEAD_H
hline(y, 1.5)

# body
for i, (name, host, cvm) in enumerate(rows):
    if i % 2 == 1:
        add(f'<rect x="{X[0]}" y="{y}" width="{R[2]-X[0]}" height="{ROW_H}" fill="#f7f8fa"/>')
    text(X[0] + 12, y + 26, name)
    for val, right in ((host, R[1]), (cvm, R[2])):
        dash = val == "—"
        text(right, y + 26, val, anchor="end", fill=GRAY if dash else INK, family=FONT if dash else MONO)
    y += ROW_H
hline(y, 2)

add('</svg>')
open(__file__.replace(".py", ".svg"), "w").write("\n".join(out) + "\n")
print("wrote", __file__.replace(".py", ".svg"))
