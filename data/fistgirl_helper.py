#!/usr/bin/env python3
"""
Coreboard — Fistgirl / FitGirl Repacks Helper
Gère la résolution des liens paste.fitgirl-repacks.site, le déchiffrement PrivateBin,
l'extraction des liens de téléchargement direct fuckingfast.co (add-on Fistgirl),
et le téléchargement reprenable avec gestion de stockage et extraction.
"""

import sys
import os
import re
import json
import time
import socket
import ssl
import subprocess
import shutil
import urllib.request
import urllib.error
import urllib.parse
from pathlib import Path

# -----------------------------------------------------------------------------
# DNS over HTTPS (DoH) / Résolution de secours Cloudflare pour fuckingfast.co
# (contourne les blocages DNS FAI fréquents en France / UE)
# -----------------------------------------------------------------------------
CF_IPS = ["104.18.2.176", "104.18.3.176"]
_orig_getaddrinfo = socket.getaddrinfo

def _doh_resolve(host):
    try:
        req = urllib.request.Request(
            f"https://cloudflare-dns.com/dns-query?name={host}&type=A",
            headers={"accept": "application/dns-json", "User-Agent": "Mozilla/5.0"}
        )
        with urllib.request.urlopen(req, timeout=4) as resp:
            data = json.loads(resp.read().decode())
            answers = [a["data"] for a in data.get("Answer", []) if a.get("type") == 1]
            if answers:
                return answers
    except Exception:
        pass
    return CF_IPS

def _custom_getaddrinfo(host, port, family=0, type=0, proto=0, flags=0):
    if host in ("fuckingfast.co", "dl.fuckingfast.co") or (isinstance(host, str) and host.endswith(".fuckingfast.co")):
        try:
            return _orig_getaddrinfo(CF_IPS[0], port, family, type, proto, flags)
        except Exception:
            pass
    return _orig_getaddrinfo(host, port, family, type, proto, flags)

socket.getaddrinfo = _custom_getaddrinfo

HEADERS_BROWSER = {
    "User-Agent": "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/125.0.0.0 Safari/537.36",
    "Accept": "text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8",
    "Accept-Language": "en-US,en;q=0.5",
}

# -----------------------------------------------------------------------------
# Extraction des liens directs (Fistgirl)
# -----------------------------------------------------------------------------
def get_fuckingfast_direct_link(ff_url):
    """
    Extrait l'URL de téléchargement direct depuis une URL fuckingfast.co
    en simulant la requête POST /f/{file_id}/go avec HX-Request (identique à fistgirl/add-on/get_ff_link.py).
    """
    m = re.search(r'fuckingfast\.co/([a-zA-Z0-9]+)(?:[#?]|$)', ff_url)
    if not m:
        raise ValueError(f"Impossible d'extraire l'identifiant de fichier depuis : {ff_url}")

    file_id = m.group(1)
    post_url = f"https://fuckingfast.co/f/{file_id}/go"

    opener = urllib.request.build_opener(urllib.request.HTTPCookieProcessor())

    # Requête préliminaire pour récupérer les cookies de session
    try:
        req = urllib.request.Request(ff_url, headers=HEADERS_BROWSER)
        with opener.open(req, timeout=8):
            pass
    except Exception:
        pass

    post_headers = {
        **HEADERS_BROWSER,
        "Accept": "*/*",
        "Content-Type": "application/x-www-form-urlencoded",
        "HX-Request": "true",
        "HX-Current-URL": ff_url,
        "HX-Target": "body",
        "Origin": "https://fuckingfast.co",
        "Referer": ff_url,
    }

    try:
        req = urllib.request.Request(post_url, data=b"", headers=post_headers, method="POST")
        with opener.open(req, timeout=12) as resp:
            hx = resp.headers.get("hx-redirect") or resp.headers.get("HX-Redirect")
            if hx:
                return hx.strip()
    except urllib.error.HTTPError as e:
        hx = e.headers.get("hx-redirect") or e.headers.get("HX-Redirect")
        if hx:
            return hx.strip()
        raise RuntimeError(f"Erreur HTTP {e.code} lors de la résolution fuckingfast: {e.reason}")
    except Exception as e:
        raise RuntimeError(f"Échec de résolution fuckingfast: {e}")

    raise RuntimeError("En-tête HX-Redirect absent de la réponse fuckingfast")

# -----------------------------------------------------------------------------
# Déchiffrement PrivateBin (FitGirl Paste)
# -----------------------------------------------------------------------------
def decrypt_fitgirl_paste(paste_url):
    """
    Télécharge et déchiffre le paste PrivateBin de paste.fitgirl-repacks.site
    via l'API Fistgirl PrivateBin decrypt (ou parsing direct).
    """
    hash_idx = paste_url.find("#")
    if hash_idx == -1:
        raise ValueError("URL de paste invalide (clé manquante après #)")
    key = paste_url[hash_idx + 1:]

    m = re.search(r'[?&]([a-f0-9]{16,32})', paste_url)
    if not m:
        m = re.search(r'[?&]pasteid=([a-f0-9]+)', paste_url)
    if not m:
        raise ValueError("Identifiant de paste introuvable dans l'URL")
    paste_id = m.group(1)

    # Récupération du JSON chiffré depuis paste.fitgirl-repacks.site
    fetch_url = f"https://paste.fitgirl-repacks.site/?{paste_id}"
    req = urllib.request.Request(
        fetch_url,
        headers={
            "X-Requested-With": "JSONHttpRequest",
            "User-Agent": HEADERS_BROWSER["User-Agent"],
            "Accept": "application/json, text/javascript, */*; q=0.01"
        }
    )
    with urllib.request.urlopen(req, timeout=12) as resp:
        paste_json = json.loads(resp.read().decode())

    adata = paste_json.get("adata", [None])[0]
    ct = paste_json.get("ct", "")
    if not adata or not ct:
        raise ValueError("Format de paste chiffré invalide ou corrompu")

    # Appel au service de déchiffrement Fistgirl (Vercel)
    decrypt_payload = json.dumps({
        "key": key,
        "data": [adata, "markdown", 0, 0],
        "cipherMessage": ct
    }).encode("utf-8")

    dreq = urllib.request.Request(
        "https://privatebin-decrypt-api-kappa.vercel.app/api/decrypt",
        data=decrypt_payload,
        headers={"Content-Type": "application/json", "User-Agent": HEADERS_BROWSER["User-Agent"]}
    )
    with urllib.request.urlopen(dreq, timeout=15) as resp:
        dec = json.loads(resp.read().decode())

    if not dec.get("success") or not dec.get("decryptedText"):
        err = dec.get("error", "Déchiffrement échoué")
        raise RuntimeError(f"Erreur API déchiffrement Fistgirl: {err}")

    return dec["decryptedText"]

# -----------------------------------------------------------------------------
# Résolution d'une source (Paste, fuckingfast direct, ou page de jeu FitGirl)
# -----------------------------------------------------------------------------
def resolve_game_source(source_url):
    """
    Prend en entrée une URL de paste FitGirl, une URL fuckingfast.co directe,
    ou une URL de page de jeu fitgirl-repacks.site,
    et renvoie les métadonnées et la liste des fichiers avec leurs liens.
    """
    links = []
    game_title = "Jeu FitGirl"

    # Si c'est une URL de page fitgirl-repacks.site (ex: https://fitgirl-repacks.site/elden-ring/)
    if "fitgirl-repacks.site" in source_url and "paste.fitgirl-repacks.site" not in source_url:
        req = urllib.request.Request(source_url, headers=HEADERS_BROWSER)
        with urllib.request.urlopen(req, timeout=12) as resp:
            html = resp.read().decode("utf-8", errors="replace")
        
        # Extrait le titre
        m_t = re.search(r'<h1 class="entry-title"[^>]*>(.*?)</h1>', html)
        if m_t:
            game_title = html_unescape(re.sub(r'<[^>]+>', '', m_t.group(1)).strip())
            
        # Cherche les liens de paste fuckingfast
        pastes = re.findall(r'https?://paste\.fitgirl-repacks\.site/\?[^\s"\'<>]+', html)
        if pastes:
            source_url = pastes[0] # Utilise le premier paste
        else:
            ffs = re.findall(r'https?://fuckingfast\.co/[^\s"\'<>]+', html)
            if ffs:
                links = list(dict.fromkeys(ffs))

    if "paste.fitgirl-repacks.site" in source_url:
        plaintext = decrypt_fitgirl_paste(source_url)
        # Trouve tous les liens fuckingfast.co
        found = re.findall(r'https?://fuckingfast\.co/[^\s"\'<>]+', plaintext)
        # Nettoie les liens
        for l in found:
            clean_l = l.rstrip(".,;)\"'>")
            if clean_l not in links:
                links.append(clean_l)
    elif "fuckingfast.co" in source_url:
        links.append(source_url.strip())

    if not links:
        raise ValueError("Aucun lien fuckingfast.co trouvé dans cette source")

    # Détecte le titre du jeu à partir du premier fragment d'URL
    if not game_title or game_title == "Jeu FitGirl":
        first_link = links[0]
        m_name = re.search(r'#([^#]+)$', first_link)
        if m_name:
            raw_name = m_name.group(1)
            raw_name = re.sub(r'_*--_*fitgirl-repacks\.site_*--_*', '', raw_name, flags=re.I)
            raw_name = re.sub(r'\.part\d+\.rar$', '', raw_name, flags=re.I)
            raw_name = re.sub(r'\.rar$', '', raw_name, flags=re.I)
            game_title = raw_name.replace("_", " ").strip()
    if not game_title:
        game_title = "Téléchargement FitGirl"

    # Prépare la liste des fichiers
    files = []
    for idx, l in enumerate(links):
        fname = f"part{idx+1:02d}.bin"
        m_f = re.search(r'#([^#]+)$', l)
        if m_f:
            fname = m_f.group(1)
            # Nettoie pour le nom de fichier local
            fname = re.sub(r'--_*fitgirl-repacks\.site_*--', '', fname, flags=re.I)
            fname = re.sub(r'^[_\s]+|[_\s]+$', '', fname)
        files.append({
            "index": idx,
            "name": fname,
            "source_url": l,
            "direct_url": "", # Sera résolu à la volée avant de télécharger
            "size": 0
        })

    return {
        "title": game_title,
        "source_url": source_url,
        "file_count": len(files),
        "files": files
    }

# -----------------------------------------------------------------------------
# Recherche dans le catalogue FitGirl Repacks
# -----------------------------------------------------------------------------
def search_fitgirl(query):
    """
    Recherche des repacks sur fitgirl-repacks.site
    Renvoie une liste de résultats avec titre, URL de l'article, et paste si disponible.
    """
    results = []
    search_url = f"https://fitgirl-repacks.site/?s={urllib.parse.quote_plus(query)}"
    req = urllib.request.Request(search_url, headers=HEADERS_BROWSER)
    try:
        with urllib.request.urlopen(req, timeout=10) as resp:
            html = resp.read().decode("utf-8", errors="replace")
    except Exception:
        feed_url = "https://fitgirl-repacks.site/feed/"
        req_f = urllib.request.Request(feed_url, headers=HEADERS_BROWSER)
        with urllib.request.urlopen(req_f, timeout=10) as resp:
            html = resp.read().decode("utf-8", errors="replace")

    matches = re.findall(r'<h1 class="entry-title"><a href="(https://fitgirl-repacks\.site/[^"]+)"[^>]*>(.*?)</a></h1>', html)
    for url, raw_title in matches:
        title = html_unescape(re.sub(r'<[^>]+>', '', raw_title).strip())
        if any(skip in title.lower() for skip in ["upcoming", "digest", "troubleshooting", "faq"]):
            continue
        results.append({
            "title": title,
            "page_url": url,
            "paste_url": url,
            "has_paste": True
        })

    return results

def html_unescape(s):
    import html
    return html.unescape(s)

# -----------------------------------------------------------------------------
# Vérification de l'espace de stockage et permissions
# -----------------------------------------------------------------------------
def check_storage(directory_path, required_bytes=0):
    """
    Vérifie l'existence, les droits d'écriture et l'espace libre du dossier cible.
    """
    p = Path(directory_path).expanduser().resolve()
    try:
        p.mkdir(parents=True, exist_ok=True)
        writable = os.access(str(p), os.W_OK)
        # Test réel d'écriture d'un fichier temporaire
        if writable:
            test_file = p / ".coreboard_write_test"
            try:
                test_file.write_text("ok")
                test_file.unlink()
            except Exception:
                writable = False
        stat = os.statvfs(str(p))
        free_bytes = stat.f_bavail * stat.f_frsize
        total_bytes = stat.f_blocks * stat.f_frsize
        has_space = free_bytes >= required_bytes if required_bytes > 0 else True

        return {
            "path": str(p),
            "exists": p.exists(),
            "writable": writable,
            "free_bytes": free_bytes,
            "total_bytes": total_bytes,
            "free_gb": round(free_bytes / (1024**3), 2),
            "total_gb": round(total_bytes / (1024**3), 2),
            "has_space": has_space
        }
    except Exception as e:
        return {
            "path": str(p),
            "exists": False,
            "writable": False,
            "free_bytes": 0,
            "total_bytes": 0,
            "free_gb": 0,
            "total_gb": 0,
            "has_space": False,
            "error": str(e)
        }

# -----------------------------------------------------------------------------
# Téléchargeur multi-fichiers avec reprise (Range) et rapport JSON en direct
# -----------------------------------------------------------------------------
def download_stream(direct_url, target_file, file_name, file_idx, total_files, done_before=0, total_all=0):
    """
    Télécharge un fichier avec support HTTP Range et émet des lignes JSON pour l'UI.
    """
    target = Path(target_file)
    existing_bytes = target.stat().st_size if target.exists() else 0

    headers = dict(HEADERS_BROWSER)
    if existing_bytes > 0:
        headers["Range"] = f"bytes={existing_bytes}-"

    req = urllib.request.Request(direct_url, headers=headers)
    t0 = time.time()
    last_emit = t0
    bytes_this_session = 0

    try:
        with urllib.request.urlopen(req, timeout=20) as resp:
            content_length = resp.headers.get("Content-Length")
            file_total = int(content_length) + existing_bytes if content_length else 0

            # Si le serveur renvoie 200 au lieu de 206 (ne supporte pas Range), repartir de zéro
            mode = "ab" if resp.status == 206 else "wb"
            if resp.status == 200 and existing_bytes > 0:
                existing_bytes = 0

            with open(target, mode) as f:
                while True:
                    chunk = resp.read(128 * 1024) # Chunks de 128 Ko
                    if not chunk:
                        break
                    f.write(chunk)
                    bytes_this_session += len(chunk)
                    current_done = existing_bytes + bytes_this_session

                    now = time.time()
                    if now - last_emit >= 0.5: # Met à jour toutes les 500ms
                        speed = bytes_this_session / max(0.1, now - t0)
                        pct = (current_done / file_total * 100.0) if file_total > 0 else 0
                        emit_json({
                            "event": "progress",
                            "file": file_name,
                            "file_index": file_idx,
                            "file_count": total_files,
                            "file_done": current_done,
                            "file_total": file_total,
                            "game_done": done_before + current_done,
                            "game_total": total_all,
                            "speed_bps": speed,
                            "pct": round(pct, 1)
                        })
                        last_emit = now

        return target.stat().st_size
    except Exception as e:
        raise RuntimeError(f"Erreur téléchargement de {file_name}: {e}")

def run_download_job(resolved_json_path, target_dir):
    """
    Lit le fichier de description résolu et télécharge séquentiellement chaque partie.
    """
    with open(resolved_json_path, "r", encoding="utf-8") as f:
        job = json.load(f)

    target_path = Path(target_dir).expanduser().resolve()
    target_path.mkdir(parents=True, exist_ok=True)

    files = job.get("files", [])
    total_files = len(files)
    game_title = job.get("title", "Jeu")

    emit_json({"event": "job_start", "title": game_title, "file_count": total_files, "dest": str(target_path)})

    total_done = 0
    for idx, item in enumerate(files):
        fname = item["name"]
        local_dest = target_path / fname
        source_url = item.get("source_url", "")
        direct_url = item.get("direct_url", "")

        # Si le fichier complet existe déjà
        if local_dest.exists() and local_dest.stat().st_size > 10 * 1024 * 1024:
            # Vérifie si le fichier suivant existe ou si c'est déjà fini
            emit_json({"event": "file_cached", "file": fname, "file_index": idx, "file_count": total_files})
            total_done += local_dest.stat().st_size
            continue

        # Résolution du lien direct à la volée pour éviter les URLs expirées
        if not direct_url:
            emit_json({"event": "resolving_link", "file": fname, "file_index": idx})
            direct_url = get_fuckingfast_direct_link(source_url)
            item["direct_url"] = direct_url

        emit_json({"event": "file_start", "file": fname, "file_index": idx, "file_count": total_files})
        fsize = download_stream(direct_url, local_dest, fname, idx, total_files, done_before=total_done)
        total_done += fsize
        emit_json({"event": "file_done", "file": fname, "file_index": idx, "file_count": total_files})

    emit_json({"event": "job_completed", "title": game_title, "total_bytes": total_done, "dest": str(target_path)})

# -----------------------------------------------------------------------------
# Extraction des archives et détection de setup.exe
# -----------------------------------------------------------------------------
def extract_archives(download_dir, extract_dir=None):
    """
    Détecte et extrait les archives téléchargées (.rar, .zip, .7z) vers le dossier d'extraction.
    Recherche ensuite l'exécutable d'installation (setup.exe ou équivalent).
    """
    src = Path(download_dir).expanduser().resolve()
    if not extract_dir:
        extract_dir = src / "extracted"
    dest = Path(extract_dir).expanduser().resolve()
    dest.mkdir(parents=True, exist_ok=True)

    # Recherche la première archive (.part01.rar, .part1.rar ou .rar unique)
    archives = list(src.glob("*.rar")) + list(src.glob("*.zip")) + list(src.glob("*.7z"))
    first_archive = None

    # Trie pour trouver .part01 ou .part1 en premier
    for a in sorted(archives):
        if ".part01." in a.name or ".part1." in a.name or ".part001." in a.name:
            first_archive = a
            break
    if not first_archive and archives:
        first_archive = archives[0]

    emit_json({"event": "extract_start", "archive": str(first_archive) if first_archive else None, "dest": str(dest)})

    if first_archive:
        # Essaye 7z, puis unrar, puis bsdtar
        cmd = None
        if shutil.which("7z"):
            cmd = ["7z", "x", "-y", f"-o{dest}", str(first_archive)]
        elif shutil.which("unrar"):
            cmd = ["unrar", "x", "-y", "-o+", str(first_archive), str(dest)]
        elif shutil.which("bsdtar"):
            cmd = ["bsdtar", "-xf", str(first_archive), "-C", str(dest)]

        if cmd:
            p = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            if p.returncode != 0:
                emit_json({"event": "extract_error", "msg": f"Erreur extraction: {p.stderr[:300]}"})
        else:
            emit_json({"event": "extract_warning", "msg": "Aucun outil d'extraction (7z/bsdtar/unrar) disponible"})

    # Recherche du setup.exe ou exécutable principal
    setup_candidates = list(dest.rglob("setup*.exe")) + list(src.rglob("setup*.exe"))
    if not setup_candidates:
        setup_candidates = list(dest.rglob("*.exe")) + list(src.rglob("*.exe"))

    found_setup = str(setup_candidates[0]) if setup_candidates else ""
    emit_json({
        "event": "extract_completed",
        "dest": str(dest),
        "setup_exe": found_setup,
        "can_install": bool(found_setup)
    })
    return found_setup

def emit_json(obj):
    print(json.dumps(obj), flush=True)

# -----------------------------------------------------------------------------
# Interface ligne de commande (CLI)
# -----------------------------------------------------------------------------
def main():
    if len(sys.argv) < 2:
        print("Usage: fistgirl_helper.py <command> [args...]")
        print("Commands:")
        print("  resolve <url>                 Résout une URL paste ou fuckingfast en liste de fichiers")
        print("  search <query>                Recherche un jeu sur FitGirl Repacks")
        print("  get-link <fuckingfast_url>    Extrait le lien direct CDN depuis fuckingfast.co")
        print("  storage-check <dir> [bytes]   Vérifie espace et permissions du dossier cible")
        print("  download <job.json> <dest>    Lance le téléchargement complet avec reprise")
        print("  extract <dir> [out_dir]       Extrait les archives et trouve setup.exe")
        sys.exit(1)

    cmd = sys.argv[1]

    try:
        if cmd == "resolve":
            url = sys.argv[2]
            res = resolve_game_source(url)
            print(json.dumps(res, indent=2))

        elif cmd == "search":
            q = sys.argv[2] if len(sys.argv) > 2 else ""
            res = search_fitgirl(q)
            print(json.dumps(res, indent=2))

        elif cmd == "get-link":
            url = sys.argv[2]
            link = get_fuckingfast_direct_link(url)
            print(link)

        elif cmd == "storage-check":
            d = sys.argv[2]
            needed = int(sys.argv[3]) if len(sys.argv) > 3 else 0
            res = check_storage(d, needed)
            print(json.dumps(res, indent=2))

        elif cmd == "download":
            job_file = sys.argv[2]
            dest = sys.argv[3] if len(sys.argv) > 3 else "~/Games/Downloads"
            run_download_job(job_file, dest)

        elif cmd == "extract":
            d = sys.argv[2]
            out = sys.argv[3] if len(sys.argv) > 3 else None
            extract_archives(d, out)

        else:
            print(f"Commande inconnue: {cmd}", file=sys.stderr)
            sys.exit(1)

    except Exception as e:
        emit_json({"error": str(e), "success": False})
        sys.exit(1)

if __name__ == "__main__":
    main()
