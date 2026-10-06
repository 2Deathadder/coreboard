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
import threading
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
FF_LINK_RE = re.compile(r'https?://fuckingfast\.co/[^\s"\'<>]+')


def _ff_links(text):
    """Liens fuckingfast.co uniques, dans l'ordre d'apparition, nettoyés de la ponctuation finale."""
    out = []
    for l in FF_LINK_RE.findall(text):
        l = html_unescape(l).rstrip(".,;)\"'>")
        if l not in out:
            out.append(l)
    return out


def _is_optional(name):
    # FitGirl : « fg-optional-* » = voix d'autres langues, bonus… ; le jeu s'installe sans
    return name.lower().startswith("fg-optional-")


def resolve_game_source(source_url):
    """
    Prend en entrée une URL de paste FitGirl, une URL fuckingfast.co directe,
    ou une URL de page de jeu fitgirl-repacks.site,
    et renvoie les métadonnées et la liste des fichiers avec leurs liens.
    Une page FitGirl propose plusieurs hébergeurs (DataNodes, FuckingFast, FileKeeper…) : seuls les liens
    FuckingFast sont exploitables. Ordre : liens FuckingFast de la page, puis paste « FuckingFast », puis tous les pastes.
    """
    links = []
    game_title = "Jeu FitGirl"

    if "fitgirl-repacks.site" in source_url and "paste.fitgirl-repacks.site" not in source_url:
        req = urllib.request.Request(source_url, headers=HEADERS_BROWSER)
        with urllib.request.urlopen(req, timeout=15) as resp:
            html = resp.read().decode("utf-8", errors="replace")

        m_t = re.search(r'<h1 class="entry-title"[^>]*>(.*?)</h1>', html, re.S)
        if m_t:
            game_title = html_unescape(re.sub(r'<[^>]+>', '', m_t.group(1)).strip())

        links = _ff_links(html)
        if not links:
            pastes = [html_unescape(u) for u in re.findall(r'https?://paste\.fitgirl-repacks\.site/\?[^\s"\'<>]+', html)]
            pastes = list(dict.fromkeys(pastes))
            labelled = [html_unescape(u) for u in re.findall(
                r'<a[^>]+href="(https?://paste\.fitgirl-repacks\.site/\?[^"]+)"[^>]*>[^<]*FuckingFast', html, re.I)]
            for p in labelled + [p for p in pastes if p not in labelled]:
                try:
                    links = _ff_links(decrypt_fitgirl_paste(p))
                except Exception:
                    continue
                if links:
                    break
    elif "paste.fitgirl-repacks.site" in source_url:
        links = _ff_links(decrypt_fitgirl_paste(source_url))
    elif "fuckingfast.co" in source_url:
        links = [source_url.strip()]

    if not links:
        raise ValueError("Aucun lien FuckingFast pour ce repack (seuls d'autres hébergeurs sont proposés)")

    if not game_title or game_title == "Jeu FitGirl":
        m_name = re.search(r'#([^#]+)$', links[0])
        if m_name:
            raw_name = m_name.group(1)
            raw_name = re.sub(r'_*--_*fitgirl-repacks\.site_*--_*', '', raw_name, flags=re.I)
            raw_name = re.sub(r'\.part\d+\.rar$', '', raw_name, flags=re.I)
            raw_name = re.sub(r'\.rar$', '', raw_name, flags=re.I)
            game_title = raw_name.replace("_", " ").strip()
    if not game_title:
        game_title = "Téléchargement FitGirl"

    files, optional = [], []
    for l in links:
        idx = len(files) + len(optional)
        fname = f"part{idx+1:02d}.bin"
        m_f = re.search(r'#([^#]+)$', l)
        if m_f:
            fname = m_f.group(1).strip()        # nom d'origine : les volumes RAR s'enchaînent par leur nom
        entry = {"name": fname, "source_url": l, "direct_url": ""}
        (optional if _is_optional(fname) else files).append(entry)
    for i, f in enumerate(files):
        f["index"] = i

    return {
        "title": game_title,
        "source_url": source_url,
        "file_count": len(files),
        "files": files,
        "optional_files": optional,
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
# Téléchargeur segmenté (même méthode que Flux)
#   - sonde de taille par « Range: bytes=0-0 » ;
#   - fichier « .part » préalloué, écrit par plusieurs connexions en parallèle (une plage d'octets chacune) ;
#   - découpage dynamique : une connexion libre prend la moitié du plus gros reste ;
#   - relances progressives par segment, lien direct re-résolu s'il a expiré ;
#   - repli sur une seule connexion si le serveur ignore les plages ;
#   - état des segments sauvegardé (« .cbseg ») : reprise exacte après coupure, annulation ou redémarrage.
# -----------------------------------------------------------------------------
SEG_CONNS = max(1, min(32, int(os.environ.get("COREBOARD_FG_CONNS", "8"))))
MIN_SEG = 4 << 20            # pas de segment initial sous 4 Mo
SPLIT_MIN = 8 << 20          # reste minimal pour un découpage dynamique
MAX_RETRIES = 8
CHUNK = 256 << 10


class LinkExpired(Exception):
    pass


class NoRanges(Exception):
    pass


def probe(url):
    """Renvoie (taille, plages_acceptées). Taille -1 si inconnue."""
    req = urllib.request.Request(url, headers={**HEADERS_BROWSER, "Range": "bytes=0-0"})
    try:
        with urllib.request.urlopen(req, timeout=20) as r:
            if r.status == 206:
                m = re.search(r'/(\d+)\s*$', r.headers.get("Content-Range", ""))
                if m:
                    return int(m.group(1)), True
            cl = r.headers.get("Content-Length")
            return (int(cl) if cl and r.status == 200 else -1), False
    except urllib.error.HTTPError as e:
        if e.code in (401, 403, 404, 410):
            raise LinkExpired(f"HTTP {e.code}") from e
        raise


class Seg:
    __slots__ = ("pos", "end", "active", "retries", "retry_at", "err")

    def __init__(self, pos, end):
        self.pos, self.end = pos, end
        self.active, self.retries, self.retry_at, self.err = False, 0, 0.0, None


class SegmentedDownload:
    def __init__(self, target, get_url, file_name, file_idx, total_files, done_before):
        self.target = Path(target)
        self.part = Path(str(target) + ".part")
        self.state = Path(str(target) + ".cbseg")
        self.get_url = get_url
        self.file_name, self.file_idx, self.total_files, self.done_before = file_name, file_idx, total_files, done_before
        self.lock = threading.Lock()
        self.stop = False
        self.segs = []
        self.size = -1
        self.ranges = False
        self.url = None
        self.fd = -1

    # --- état persistant
    def _load_state(self):
        try:
            st = json.loads(self.state.read_text())
            if st.get("size") == self.size and self.part.exists() and self.part.stat().st_size == self.size:
                return [Seg(int(p), int(e)) for p, e in st["segs"] if int(p) < int(e)]
        except Exception:
            pass
        return None

    def _save_state(self):
        with self.lock:
            segs = [[s.pos, s.end] for s in self.segs if s.pos < s.end]
        tmp = Path(str(self.state) + ".tmp")
        tmp.write_text(json.dumps({"size": self.size, "segs": segs}))
        os.replace(tmp, self.state)

    def _layout(self):
        n = max(1, min(SEG_CONNS, self.size // MIN_SEG))
        per = self.size // n
        return [Seg(i * per, self.size if i == n - 1 else (i + 1) * per) for i in range(n)]

    # --- une connexion
    def _worker(self, seg):
        try:
            with self.lock:
                start, end = seg.pos, seg.end
            headers = dict(HEADERS_BROWSER)
            if self.ranges:
                headers["Range"] = f"bytes={start}-{end - 1}"
            req = urllib.request.Request(self.url, headers=headers)
            try:
                r = urllib.request.urlopen(req, timeout=30)
            except urllib.error.HTTPError as e:
                if e.code in (401, 403, 404, 410):
                    raise LinkExpired(f"HTTP {e.code}") from e
                raise
            with r:
                if self.ranges and r.status != 206:
                    raise NoRanges()
                while not self.stop:
                    chunk = r.read(CHUNK)
                    if not chunk:
                        break
                    with self.lock:
                        room = seg.end - seg.pos
                        if room <= 0 or seg not in self.segs:   # segment abandonné (repli en flux unique)
                            break
                        data = chunk[:room] if len(chunk) > room else chunk
                        os.pwrite(self.fd, data, seg.pos)
                        seg.pos += len(data)
                        if seg.pos >= seg.end:
                            break
            with self.lock:
                if seg not in self.segs:
                    return
                if seg.end == float("inf") and not self.stop:     # taille inconnue : fin du flux = fin du fichier
                    seg.end = seg.pos
                elif seg.pos < seg.end and not self.stop:
                    raise ConnectionError("connexion coupée avant la fin du segment")
        except Exception as e:  # noqa: BLE001 — traité par la boucle principale
            seg.err = e
        finally:
            seg.active = False

    def _start(self, seg):
        seg.active, seg.err = True, None
        threading.Thread(target=self._worker, args=(seg,), daemon=True).start()

    def _done_bytes(self):
        if self.size <= 0:
            return sum(s.pos for s in self.segs)
        return self.size - sum(max(0, s.end - s.pos) for s in self.segs)

    def run(self):
        self.url = self.get_url(False)
        for attempt in range(4):
            try:
                self.size, self.ranges = probe(self.url)
                break
            except LinkExpired:
                self.url = self.get_url(True)
            except Exception:
                if attempt == 3:
                    raise
                time.sleep(2 * (attempt + 1))
        if self.size <= 0:
            self.ranges = False

        # fichier déjà complet, ou téléchargé partiellement par l'ancienne méthode (flux unique)
        if self.target.exists() and not self.part.exists():
            have = self.target.stat().st_size
            if self.size > 0 and have == self.size:
                return have, True
            if self.ranges and 0 < have < self.size:
                os.replace(self.target, self.part)
                self.segs = [Seg(have, self.size)]
        if not self.segs:
            self.segs = (self._load_state() if self.ranges else None) or \
                        (self._layout() if self.ranges else [Seg(0, self.size if self.size > 0 else float("inf"))])

        self.fd = os.open(self.part, os.O_RDWR | os.O_CREAT | os.O_CLOEXEC, 0o644)
        try:
            if not self.ranges:
                os.ftruncate(self.fd, 0)
            elif os.fstat(self.fd).st_size != self.size:
                os.ftruncate(self.fd, self.size)
                try:
                    os.posix_fallocate(self.fd, 0, self.size)   # réserve l'espace : pas d'échec en cours de route
                except OSError:
                    pass
            self._loop()
            os.fsync(self.fd)
        finally:
            os.close(self.fd)
            self.fd = -1

        done = self._done_bytes()
        if self.size > 0 and done < self.size:
            raise RuntimeError("téléchargement incomplet")
        os.replace(self.part, self.target)
        try:
            self.state.unlink()
        except FileNotFoundError:
            pass
        return self.target.stat().st_size, False

    def _loop(self):
        last_emit = t_prev = time.time()
        last_save = 0.0                                     # premier état écrit tout de suite
        done_prev = self._done_bytes()
        speed = 0.0
        relink = False
        while True:
            now = time.time()
            fatal = None
            with self.lock:
                for s in self.segs:                         # bilan des connexions terminées
                    if s.active or s.err is None:
                        continue
                    e, s.err = s.err, None
                    if isinstance(e, NoRanges):             # le serveur ignore les plages : une seule connexion
                        self.ranges = False
                        self.segs = [Seg(0, self.size if self.size > 0 else float("inf"))]
                        os.ftruncate(self.fd, 0)
                        break
                    if isinstance(e, LinkExpired):
                        relink = True
                    s.retries += 1
                    if s.retries > MAX_RETRIES:
                        fatal = e
                    s.retry_at = now + min(30, 2 ** s.retries)
                    if not self.ranges and s.pos > 0:       # flux unique non reprenable : on repart de zéro
                        s.pos = 0
                        os.ftruncate(self.fd, 0)
                pending = [s for s in self.segs if s.pos < s.end]
                if not pending and not any(s.active for s in self.segs):
                    break
            if fatal is not None:
                self.stop = True
                self._save_state()
                raise RuntimeError(f"échec après {MAX_RETRIES} tentatives : {fatal}")
            if relink:
                relink = False
                try:
                    self.url = self.get_url(True)
                except Exception:
                    pass

            with self.lock:
                active = sum(s.active for s in self.segs)
                conns = SEG_CONNS if self.ranges else 1
                for s in self.segs:
                    if active >= conns:
                        break
                    if not s.active and s.pos < s.end and s.retry_at <= now:
                        self._start(s)
                        active += 1
                # découpage dynamique : une connexion libre prend la moitié du plus gros reste
                while self.ranges and active < conns:
                    big = max((s for s in self.segs if s.active and s.end - s.pos > SPLIT_MIN),
                              key=lambda s: s.end - s.pos, default=None)
                    if big is None:
                        break
                    mid = big.pos + (big.end - big.pos) // 2
                    n = Seg(mid, big.end)
                    big.end = mid
                    self.segs.append(n)
                    self._start(n)
                    active += 1

            if now - last_emit >= 0.5:
                done = self._done_bytes()
                dt = max(0.05, now - t_prev)
                speed = speed * 0.5 + 0.5 * max(0, done - done_prev) / dt
                done_prev, t_prev = done, now
                total = self.size if self.size > 0 else 0
                emit_json({
                    "event": "progress",
                    "file": self.file_name,
                    "file_index": self.file_idx,
                    "file_count": self.total_files,
                    "file_done": done,
                    "file_total": total,
                    "game_done": self.done_before + done,
                    # estimation : les parties FitGirl ont toutes la même taille (sauf la dernière)
                    "game_total": self.done_before + total * (self.total_files - self.file_idx) if total else 0,
                    "speed_bps": speed,
                    "conns": active,
                    "pct": round(done * 100.0 / total, 1) if total else 0,
                })
                last_emit = now
            if self.ranges and now - last_save >= 1:
                self._save_state()
                last_save = now
            time.sleep(0.2)


def safe_filename(name, idx):
    """
    Nom local sûr : le nom vient de l'URL distante (fragment #...) et ne doit jamais sortir du dossier cible.
    """
    name = urllib.parse.unquote(str(name)).replace("\\", "/")
    name = name.rsplit("/", 1)[-1].strip().lstrip(".")
    name = re.sub(r'[\x00-\x1f]', '', name)
    return name or f"part{idx+1:02d}.bin"

def run_download_job(resolved_json_path, target_dir):
    """
    Lit le fichier de description résolu et télécharge chaque partie (segmentée, reprenable).
    """
    with open(resolved_json_path, "r", encoding="utf-8") as f:
        job = json.load(f)

    target_path = Path(target_dir).expanduser().resolve()
    target_path.mkdir(parents=True, exist_ok=True)

    files = job.get("files", [])
    total_files = len(files)
    game_title = job.get("title", "Jeu")

    emit_json({"event": "job_start", "title": game_title, "file_count": total_files, "dest": str(target_path),
               "conns": SEG_CONNS})

    total_done = 0
    for idx, item in enumerate(files):
        fname = safe_filename(item.get("name", ""), idx)
        source_url = item.get("source_url", "")

        def get_url(force, item=item, fname=fname, idx=idx):
            # lien direct résolu à la demande (il expire) ; force = re-résolution après un refus du serveur
            if force or not item.get("direct_url"):
                emit_json({"event": "resolving_link", "file": fname, "file_index": idx})
                item["direct_url"] = get_fuckingfast_direct_link(source_url)
            return item["direct_url"]

        emit_json({"event": "file_start", "file": fname, "file_index": idx, "file_count": total_files})
        try:
            fsize, cached = SegmentedDownload(target_path / fname, get_url, fname, idx, total_files, total_done).run()
        except Exception as e:
            raise RuntimeError(f"{fname} : {e}") from e
        total_done += fsize
        emit_json({"event": "file_cached" if cached else "file_done",
                   "file": fname, "file_index": idx, "file_count": total_files})

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
            print(json.dumps(res))

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
