#!/usr/bin/env python3
"""Generate rx-dma-datapath.svg: NIC Rx DMA data path (original layout, English labels)."""

FONT = "Helvetica, Arial, 'Liberation Sans', sans-serif"
MONO = "'Courier New', 'Liberation Mono', monospace"
BLUE = "#2e5c9e"      # CPU-side arrows / box
ORANGE = "#c0501a"    # NIC-side (DMA) arrows / labels
PEACH = "#fce4d6"     # occupied slot fill
PANEL = "#eceef2"     # host-memory panel
INK = "#222222"
GRAY = "#595959"
W, H = 1920, 800

out = []
def add(s): out.append(s)

def rect(x, y, w, h, fill="#fff", stroke=INK, sw=1.5, rx=0, dash=None):
    d = f' stroke-dasharray="{dash}"' if dash else ""
    add(f'<rect x="{x}" y="{y}" width="{w}" height="{h}" rx="{rx}" fill="{fill}" stroke="{stroke}" stroke-width="{sw}"{d}/>')

def text(x, y, s, size=18, anchor="middle", weight="normal", fill=INK, family=FONT, style="normal"):
    add(f'<text x="{x}" y="{y}" font-family="{family}" font-size="{size}" font-weight="{weight}" '
        f'font-style="{style}" text-anchor="{anchor}" fill="{fill}">{s}</text>')

def arrow(pts, color, sw=2.5):
    p = " ".join(f"{x},{y}" for x, y in pts)
    add(f'<polyline points="{p}" fill="none" stroke="{color}" stroke-width="{sw}" '
        f'stroke-linejoin="round" marker-end="url(#arrow-{ "blue" if color == BLUE else "orange" })"/>')

def badge(x, y, n, color):
    add(f'<circle cx="{x}" cy="{y}" r="12" fill="#fff" stroke="{color}" stroke-width="2"/>')
    text(x, y + 6, str(n), size=16, weight="bold", fill=color)

def ring(x0, y, n, w, h, shaded):
    for i in range(n):
        rect(x0 + i * w, y, w, h, fill=PEACH if i in shaded else "#fff")
        if i in shaded:
            text(x0 + i * w + w / 2, y + h / 2 + 6, str(i), size=18, family=MONO)

add(f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" viewBox="0 0 {W} {H}">')
add(f'''<defs>
  <marker id="arrow-blue" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="14" markerHeight="14" markerUnits="userSpaceOnUse" orient="auto">
    <path d="M0,0 L10,5 L0,10 z" fill="{BLUE}"/></marker>
  <marker id="arrow-orange" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="14" markerHeight="14" markerUnits="userSpaceOnUse" orient="auto">
    <path d="M0,0 L10,5 L0,10 z" fill="{ORANGE}"/></marker>
</defs>''')
add(f'<rect width="{W}" height="{H}" fill="#fff"/>')

# ---------------- Software (CPU) ----------------
add('<g id="cpu">')
rect(60, 300, 268, 250, stroke=BLUE, sw=2.5, rx=8)
text(194, 400, "Software (CPU)", size=22, weight="bold")
text(194, 440, "DPDK PMD", size=18)
text(194, 472, "polls continuously,", size=17, fill=GRAY)
text(194, 496, "no interrupts", size=17, fill=GRAY)
add('</g>')

# ---------------- Host memory panel ----------------
add('<g id="host-memory">')
rect(400, 45, 1090, 720, fill=PANEL, stroke="none", rx=12)
text(430, 92, "Four things in host memory (everything the NIC accesses by DMA lives here)",
     size=20, weight="bold", fill=GRAY, anchor="start")

# A. free buffers
add('<g id="buffers">')
text(460, 140, "A. Free buffers: a pool of memory blocks waiting for packets, typically 2 KB each",
     size=18, anchor="start")
for x in (460, 670):
    rect(x, 157, 190, 76)
    text(x + 95, 202, "empty", size=18, fill=GRAY)
rect(880, 157, 190, 76, fill=PEACH)
text(975, 202, "packet, 1500 B", size=18)
add('</g>')

# B. RQ
add('<g id="rq">')
text(460, 300, "B. Work-request ring (RQ): each slot says “put the packet at this address” {addr, len, lkey}",
     size=18, anchor="start")
ring(460, 318, 8, 76, 74, {0, 1, 2})
add('</g>')

# C. CQ
add('<g id="cq">')
text(460, 462, "C. Completion ring (CQ): the NIC writes one slot per received packet {len, WQE index, status, timestamp}",
     size=18, anchor="start")
ring(460, 485, 8, 76, 74, {0})
add('</g>')

# D. doorbell record
add('<g id="doorbell-record">')
text(460, 632, "D. Doorbell record: one integer, “how far I have written into the work-request ring”",
     size=18, anchor="start")
rect(460, 652, 305, 76)
text(612, 700, "producer = 3", size=22, family=MONO)
text(790, 700, "The NIC reads it to learn how many free buffers remain", size=17, fill=GRAY, anchor="start")
add('</g>')
add('</g>')  # host-memory

# ---------------- NIC ----------------
add('<g id="nic">')
rect(1548, 45, 345, 720, stroke=INK, sw=2.5, rx=10)
text(1720, 98, "NIC", size=22, weight="bold")

rect(1578, 135, 285, 95, rx=4)
text(1720, 172, "Doorbell register", size=20, weight="bold")
text(1720, 202, "on the NIC; CPU writes it via MMIO", size=15, fill=GRAY)

rect(1578, 262, 285, 112, rx=4)
text(1720, 297, "Address book (mkey table)", size=20, weight="bold")
text(1720, 327, "lkey → which physical pages", size=16, fill=GRAY)
text(1720, 352, "a memory region maps to", size=16, fill=GRAY)

rect(1578, 408, 285, 105, rx=4, dash="8,6")
text(1720, 443, "DMA engine", size=20, weight="bold")
text(1720, 473, "reads/writes host memory after", size=16, fill=GRAY)
text(1720, 497, "translating via the address book", size=16, fill=GRAY)

text(1720, 600, "Wire  ↓ ↑", size=20, fill=GRAY)
add('</g>')

# ---------------- CPU-side arrows (blue) ----------------
add('<g id="cpu-arrows">')
arrow([(328, 355), (452, 355)], BLUE)                       # 1: post work requests
badge(392, 330, 1, BLUE)
arrow([(194, 550), (194, 690), (452, 690)], BLUE)           # 2: update doorbell record
badge(222, 620, 2, BLUE)
arrow([(452, 522), (336, 522)], BLUE)                       # 6: poll the CQ
badge(392, 498, 6, BLUE)
add('</g>')

# ---------------- NIC-side arrows (orange) ----------------
add('<g id="nic-arrows">')
# 4: DMA writes the packet into a buffer
arrow([(1548, 195), (1078, 195)], ORANGE)
badge(1105, 168, 4, ORANGE)
text(1125, 174, "DMA writes the packet into a buffer", size=18, weight="bold", fill=ORANGE, anchor="start")
# 3: DMA reads work requests + doorbell record
arrow([(1548, 355), (1078, 355)], ORANGE)
badge(1105, 331, 3, ORANGE)
text(1125, 326, "DMA reads the work requests", size=18, weight="bold", fill=ORANGE, anchor="start")
text(1125, 347, "and the doorbell record", size=18, weight="bold", fill=ORANGE, anchor="start")
text(1105, 384, "circular: wraps around at the end", size=16, fill=GRAY, anchor="start", style="italic")
# 5: DMA writes one completion entry
arrow([(1548, 522), (1078, 522)], ORANGE)
badge(1105, 495, 5, ORANGE)
text(1125, 501, "DMA writes one completion entry", size=18, weight="bold", fill=ORANGE, anchor="start")
text(1105, 551, "each slot is 64 bytes, called a CQE", size=16, fill=GRAY, anchor="start", style="italic")
add('</g>')

add('</svg>')
open(__file__.replace(".py", ".svg"), "w").write("\n".join(out) + "\n")
print("wrote", __file__.replace(".py", ".svg"))
