#!/usr/bin/env python3
"""Generate guest-page-mapping.svg: kernel/user page tables map to one physical page; NIC DMAs to it."""

FONT = "Helvetica, Arial, 'Liberation Sans', sans-serif"
MONO = "'Courier New', 'Liberation Mono', monospace"
BLUE = "#2e5c9e"
ORANGE = "#c0501a"
PEACH = "#fce4d6"
PANEL = "#eceef2"
INK = "#222222"
GRAY = "#595959"
W, H = 1100, 420

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

add(f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" viewBox="0 0 {W} {H}">')
add(f'''<defs>
  <marker id="arrow-blue" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="14" markerHeight="14" markerUnits="userSpaceOnUse" orient="auto">
    <path d="M0,0 L10,5 L0,10 z" fill="{BLUE}"/></marker>
  <marker id="arrow-orange" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="14" markerHeight="14" markerUnits="userSpaceOnUse" orient="auto">
    <path d="M0,0 L10,5 L0,10 z" fill="{ORANGE}"/></marker>
</defs>''')
add(f'<rect width="{W}" height="{H}" fill="#fff"/>')

# ---------------- headings ----------------
text(40, 42, "Inside the guest VM", size=20, weight="bold", fill=GRAY, anchor="start")
text(940, 42, "Host side", size=20, weight="bold", fill=GRAY)

# ---------------- guest panel ----------------
add('<g id="guest">')
rect(40, 60, 680, 340, fill=PANEL, stroke="none", rx=12)

add('<g id="kernel-page-table">')
rect(70, 95, 330, 100, stroke=BLUE, sw=2.5, rx=8)
text(90, 128, "Guest kernel page table", size=20, weight="bold", anchor="start")
text(90, 158, "kernel virtual address K", size=18, anchor="start")
text(90, 182, "direct map (alloc_pages)", size=15, fill=GRAY, anchor="start")
add('</g>')

add('<g id="process-page-table">')
rect(70, 265, 330, 100, stroke=BLUE, sw=2.5, rx=8)
text(90, 298, "DPDK process page table", size=20, weight="bold", anchor="start")
text(90, 328, "user virtual address P", size=18, anchor="start")
text(90, 352, "via mmap()", size=15, fill=GRAY, anchor="start")
add('</g>')

add('<g id="physical-page">')
rect(520, 190, 170, 70, fill=PEACH, rx=4)
text(605, 232, "Physical page X", size=20, weight="bold")
add('</g>')

add('<g id="mapping-arrows">')
arrow([(400, 145), (450, 145), (450, 213), (512, 213)], BLUE)
text(425, 135, "K \u2192 X", size=16, weight="bold", fill=BLUE)
arrow([(400, 315), (450, 315), (450, 237), (512, 237)], BLUE)
text(425, 305, "P \u2192 X", size=16, weight="bold", fill=BLUE)
add('</g>')
add('</g>')  # guest

# ---------------- NIC (host side) ----------------
add('<g id="nic">')
rect(820, 160, 240, 130, stroke=INK, sw=2.5, rx=10)
text(940, 205, "NIC", size=22, weight="bold")
text(940, 240, "DMA to physical", size=16, fill=GRAY)
text(940, 262, "address X", size=16, fill=GRAY)
add('</g>')

add('<g id="dma-arrow">')
arrow([(820, 225), (698, 225)], ORANGE)
text(759, 214, "DMA", size=18, weight="bold", fill=ORANGE)
add('</g>')

add('</svg>')
open(__file__.replace(".py", ".svg"), "w").write("\n".join(out) + "\n")
print("wrote", __file__.replace(".py", ".svg"))
