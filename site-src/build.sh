#!/bin/sh
# Génère le site (docs/, publié par GitHub Pages) à partir de site-src/
set -e
cd "$(dirname "$0")/.."
WM=$(python3 site-src/wordmark.py)
python3 - "$WM" <<'PY'
import sys
t = open("site-src/index.template.html").read()
open("docs/index.html", "w").write(t.replace("{{WORDMARK}}", sys.argv[1]))
PY
mkdir -p docs/en
python3 site-src/i18n_en.py docs/index.html docs/en/index.html
DATE=$(date -u +%Y-%m-%d)
cat > docs/sitemap.xml <<XML
<?xml version="1.0" encoding="UTF-8"?>
<urlset xmlns="http://www.sitemaps.org/schemas/sitemap/0.9" xmlns:xhtml="http://www.w3.org/1999/xhtml" xmlns:image="http://www.google.com/schemas/sitemap-image/1.1">
  <url>
    <loc>https://2deathadder.github.io/coreboard/</loc><lastmod>$DATE</lastmod><changefreq>weekly</changefreq><priority>1.0</priority>
    <xhtml:link rel="alternate" hreflang="fr" href="https://2deathadder.github.io/coreboard/"/>
    <xhtml:link rel="alternate" hreflang="en" href="https://2deathadder.github.io/coreboard/en/"/>
    <image:image><image:loc>https://2deathadder.github.io/coreboard/img/og-image.jpg</image:loc></image:image>
  </url>
  <url>
    <loc>https://2deathadder.github.io/coreboard/en/</loc><lastmod>$DATE</lastmod><changefreq>weekly</changefreq><priority>0.9</priority>
    <xhtml:link rel="alternate" hreflang="fr" href="https://2deathadder.github.io/coreboard/"/>
    <xhtml:link rel="alternate" hreflang="en" href="https://2deathadder.github.io/coreboard/en/"/>
  </url>
</urlset>
XML
printf 'User-agent: *\nAllow: /\n\nSitemap: https://2deathadder.github.io/coreboard/sitemap.xml\n' > docs/robots.txt
cat > docs/404.html <<'HTML'
<!doctype html><html lang="fr"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1">
<title>Page introuvable — Coreboard</title><meta name="robots" content="noindex"><link rel="icon" href="/coreboard/favicon.svg" type="image/svg+xml">
<style>body{margin:0;min-height:100vh;display:grid;place-items:center;background:#070506;color:#f4eff0;font:15px/1.6 ui-monospace,monospace;text-align:center;padding:16px}
h1{font-size:64px;margin:0;color:#ff1a3a}a{color:#fff;border:1px solid #3d272c;padding:8px 16px;border-radius:6px;text-decoration:none;display:inline-block;margin-top:20px}a:hover{border-color:#ff1a3a}</style></head>
<body><main><h1>404</h1><p>Cette page n'existe pas.</p><a href="/coreboard/">Retour à Coreboard</a></main></body></html>
HTML
: > docs/.nojekyll
echo "site généré dans docs/"
