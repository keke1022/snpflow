#!/usr/bin/env python3
"""Generate a paper-style SVG of the DPDK/NIC Rx DMA data path."""

FONT = "Helvetica, Arial, 'Liberation Sans', sans-serif"
GRAY = "#d9d9d9"
W, H = 1040, 570

out = []
def add(s): out.append(s)

def rect(x, y, w, h, fill="#fff", dash=None, sw=1.2, rx=0, cls=None):
    d = f' stroke-dasharray="{dash}"' if dash else ""
    c = f' class="{cls}"' if cls else ""
    add(f'<rect x="{x}" y="{y}" width="{w}" height="{h}" rx="{rx}" fill="{fill}" stroke="#000" stroke-width="{sw}"{d}{c}/>')

def text(x, y, s, size=12, anchor="middle", weight="normal", style="normal", family=FONT):
    add(f'<text x="{x}" y="{y}" font-family="{family}" font-size="{size}" font-weight="{weight}" font-style="{style}" text-anchor="{anchor}" fill="#000">{s}</text>')

def arrow(pts, dash=None, sw=1.2):
    d = f' stroke-dasharray="{dash}"' if dash else ""
    p = " ".join(f"{x},{y}" for x, y in pts)
    add(f'<polyline points="{p}" fill="none" stroke="#000" stroke-width="{sw}"{d} marker-end="url(#arrow)"/>')

def badge(x, y, n):
    add(f'<circle cx="{x}" cy="{y}" r="8" fill="#fff" stroke="#000" stroke-width="1"/>')
    text(x, y + 3.5, str(n), size=10, weight="bold")

def cells(x0, y, n, w, h, shaded, labels=True):
    for i in range(n):
        rect(x0 + i * w, y, w, h, fill=GRAY if i in shaded else "#fff")
        if labels and i in shaded:
            text(x0 + i * w + w / 2, y + h / 2 + 4, str(i), size=11, family="'Courier New', 'Liberation Mono', monospace")

add(f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" viewBox="0 0 {W} {H}" font-family="{FONT}">')
add('''<defs>
  <marker id="arrow" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="9" markerHeight="9" markerUnits="userSpaceOnUse" orient="auto-start-reverse">
    <path d="M0,0 L10,5 L0,10 z" fill="#000"/>
  </marker>
</defs>''')
add(f'<rect width="{W}" height="{H}" fill="#fff"/>')

# ---------- Software (CPU) ----------
add('<g id="cpu">')
rect(20, 160, 150, 180, rx=2)
text(95, 200, "Software (CPU)", size=13, weight="bold")
text(95, 224, "DPDK poll-mode driver")
text(95, 244, "busy polling, no interrupts", size=11, style="italic")
add('</g>')

# ---------- Host memory ----------
add('<g id="host-memory">')
rect(240, 40, 560, 440, dash="6,4", rx=2)
text(252, 62, "Host memory (DMA-accessible)", size=13, weight="bold", anchor="start")

# (a) Rx buffers
add('<g id="rx-buffers">')
text(260, 90, "(a) Rx buffers (mbufs), 2 KB each", anchor="start")
for i, x in enumerate((260, 355, 450)):
    rect(x, 100, 85, 48)
    text(x + 42.5, 128, "empty", size=11, style="italic")
rect(545, 100, 85, 48, fill=GRAY)
text(587.5, 121, "packet", size=11)
text(587.5, 136, "1500 B", size=11)
add('</g>')

# (b) RQ
add('<g id="rq">')
text(260, 185, "(b) Receive queue (RQ): ring of WQEs, each {addr, len, lkey}", anchor="start")
cells(260, 195, 8, 50, 44, {0, 1, 2})
add('</g>')

# (c) CQ
add('<g id="cq">')
text(260, 285, "(c) Completion queue (CQ): ring of 64-B CQEs, each {len, WQE idx, status, timestamp}", anchor="start")
cells(260, 295, 8, 50, 44, {0})
add('</g>')

# (d) doorbell record
add('<g id="doorbell-record">')
text(260, 395, "(d) Doorbell record: RQ producer index", anchor="start")
rect(260, 405, 140, 44)
text(330, 432, "producer = 3", size=12, family="'Courier New', 'Liberation Mono', monospace")
text(415, 422, "Read by the NIC to learn how many", size=11, style="italic", anchor="start")
text(415, 437, "WQEs (free buffers) are available", size=11, style="italic", anchor="start")
add('</g>')
add('</g>')  # host-memory

# ---------- NIC ----------
add('<g id="nic">')
rect(840, 40, 170, 440, rx=2)
text(925, 62, "NIC", size=13, weight="bold")

rect(852, 85, 146, 60)
text(925, 108, "Doorbell register", weight="bold")
text(925, 126, "MMIO; written by CPU", size=11)

rect(852, 165, 146, 70)
text(925, 190, "Memory key (mkey) table", weight="bold")
text(925, 208, "lkey → physical pages", size=11)
text(925, 223, "used for address translation", size=11)

rect(852, 265, 146, 70, dash="4,3")
text(925, 292, "DMA engine", weight="bold")
text(925, 310, "reads / writes host memory", size=11)
text(925, 325, "after mkey translation", size=11)

# wire
add('<line x1="925" y1="480" x2="925" y2="520" stroke="#000" stroke-width="1.2" marker-end="url(#arrow)" marker-start="url(#arrow)"/>')
text(940, 506, "wire", size=11, anchor="start", style="italic")
add('</g>')

# ---------- Data-path arrows ----------
add('<g id="arrows">')
# 1: CPU posts WQEs
arrow([(170, 217), (252, 217)])
badge(184, 205, 1); text(196, 209, "post WQEs", size=10, anchor="start")
# 2: CPU updates doorbell record
arrow([(95, 340), (95, 427), (252, 427)])
badge(115, 380, 2); text(128, 384, "update doorbell", size=11, anchor="start")
text(128, 398, "record", size=11, anchor="start")
# 6: CPU polls CQ
arrow([(252, 317), (178, 317)])
badge(184, 305, 6); text(196, 309, "poll CQ", size=10, anchor="start")
# 3: NIC reads WQEs + doorbell record
arrow([(840, 217), (668, 217)], dash="5,3")
badge(680, 197, 3); text(694, 201, "DMA read:", size=11, anchor="start")
text(680, 235, "WQEs + doorbell record", size=11, anchor="start")
# 4: NIC writes packet
arrow([(840, 124), (638, 124)], dash="5,3")
badge(652, 110, 4); text(666, 114, "DMA write: packet payload", size=11, anchor="start")
# 5: NIC writes CQE
arrow([(840, 317), (668, 317)], dash="5,3")
badge(680, 303, 5); text(694, 307, "DMA write: CQE", size=11, anchor="start")
add('</g>')

# ---------- Legend ----------
add('<g id="legend">')
add('<line x1="240" y1="546" x2="278" y2="546" stroke="#000" stroke-width="1.2" marker-end="url(#arrow)"/>')
text(286, 550, "CPU access (load/store, MMIO)", size=11, anchor="start")
add('<line x1="470" y1="546" x2="508" y2="546" stroke="#000" stroke-width="1.2" stroke-dasharray="5,3" marker-end="url(#arrow)"/>')
text(516, 550, "NIC DMA access", size=11, anchor="start")
rect(630, 538, 16, 16, fill=GRAY)
text(654, 550, "entry in use / filled", size=11, anchor="start")
add('</g>')

add('</svg>')
open("/home/beijie/snp-dpdk/docs/figures/rx-dma-datapath.svg", "w").write("\n".join(out) + "\n")
print("ok")
