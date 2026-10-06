#!/usr/bin/env python3
"""Génère le logo « COREBOARD » en pixel art (SVG), dégradé rouge par bandes horizontales."""
G = {
 'C': ["011110","111111","110000","110000","110000","110000","111111","011110"],
 'O': ["011110","111111","110011","110011","110011","110011","111111","011110"],
 'R': ["111110","111111","110011","111111","111110","110110","110011","110011"],
 'E': ["111111","111111","110000","111110","111110","110000","111111","111111"],
 'B': ["111110","111111","110011","111110","111111","110011","111111","111110"],
 'A': ["011110","111111","110011","110011","111111","111111","110011","110011"],
 'D': ["111110","111111","110011","110011","110011","110011","111111","111110"],
}
ROWS = ["#ffe3e8", "#ffb8c3", "#ff7088", "#ff1a3a", "#e0102e", "#a80b22", "#6b0816", "#3a050c"]
P, GAP = 10, 1                      # taille d'un pixel, espace entre pixels (effet grille)
word = "COREBOARD"
rects, x = [], 0
for ch in word:
    g = G[ch]
    for r, line in enumerate(g):
        for c, v in enumerate(line):
            if v == "1":
                rects.append(f'<rect x="{x + c * P}" y="{r * P}" width="{P - GAP}" height="{P - GAP}" fill="{ROWS[r]}"/>')
    x += (len(g[0]) + 1) * P
w, h = x - P, len(ROWS) * P
print(f'<svg class="wordmark" viewBox="0 0 {w} {h}" role="img" aria-label="Coreboard" xmlns="http://www.w3.org/2000/svg">' + "".join(rects) + "</svg>")
