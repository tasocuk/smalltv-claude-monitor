#!/usr/bin/env python3
# ============================================================================
#  SmallTV Ajani (v3) - PC istatistikleri + Claude limit
#    PC (CPU/RAM)  -> POST /stats   her 2 sn
#    Claude limit  -> POST /claude  her 2 dk   (ayri thread, PC'yi bloklamaz)
#
#  v3:
#   - Cihaz IP'si degisirse kendisi bulur (smalltv.local -> son bilinen IP ->
#     alt agda /health taramasi). Bulunan IP agent/device.txt'ye yazilir.
#   - Token bos/gecersizse cihaza {"err": "..."} gonderir; ekranda ve panelde
#     "Claude girisi gerekli" gorunur (eskiden sessizce bayat veri kaliyordu).
#   - Token yenileme kilit dosyasiyla yapilir ve once dosya yeniden okunur;
#     baska bir surec (Claude Code, baska bir ajan) yenilediyse onunkini kullanir.
#   - Tek kopya calisir; ciktilar agent.log'a yazilir (pythonw'de konsol yok).
# ============================================================================
import json, time, socket, sys, os, threading, urllib.request, urllib.error
import concurrent.futures, ipaddress
from datetime import datetime

# ------------------------------ AYARLAR -------------------------------------
DEVICE_HINT     = "http://192.168.1.32"      # ilk denenecek adres; degisirse ajan kendisi bulur
PC_INTERVAL     = 2.0                        # PC verisi araligi (sn)
ENABLE_CLAUDE   = True                       # False -> Claude limiti HIC cekilmez, token'a DOKUNULMAZ
CLAUDE_INTERVAL = 120.0                      # Claude limiti araligi (sn)
# ----------------------------------------------------------------------------
#  >>> UYARI (Claude modulu) <<<
#  Bu bolum Anthropic'in BELGELENMEMIS bir ic ucunu (api/oauth/usage) senin
#  KENDI yerel OAuth token'inla cagirir. Salt-okunurdur; limitini TUKETMEZ.
#  Resmi bir API degildir - kendi hesabin, kendi riskin.
# ----------------------------------------------------------------------------
HERE       = os.path.dirname(os.path.abspath(__file__))
CRED_PATH  = os.path.join(os.path.expanduser("~"), ".claude", ".credentials.json")
LOCK_PATH  = CRED_PATH + ".smalltv.lock"
CACHE_PATH = os.path.join(HERE, "device.txt")
LOG_PATH   = os.path.join(HERE, "agent.log")
CLIENT_ID  = "9d1c250a-e61b-44d9-88ed-5944d1962f5e"          # Claude Code OAuth client_id
TOKEN_URLS = ["https://platform.claude.com/v1/oauth/token",
              "https://api.anthropic.com/v1/oauth/token"]
USAGE_URL  = "https://api.anthropic.com/api/oauth/usage"
UA         = "smalltv-agent/3"
SINGLE_PORT = 47821                                         # tek kopya kilidi

try:
    import psutil
except ImportError:
    import subprocess
    subprocess.run([sys.executable, "-m", "pip", "install", "--quiet", "psutil"]); import psutil

HOST = socket.gethostname()[:23]
device = None
_dev_lock = threading.Lock()


def log(msg):
    line = f"{datetime.now():%Y-%m-%d %H:%M:%S} {msg}"
    try:
        print(line, flush=True)
    except Exception:
        pass
    try:
        if os.path.exists(LOG_PATH) and os.path.getsize(LOG_PATH) > 256_000:
            os.replace(LOG_PATH, LOG_PATH + ".1")
        with open(LOG_PATH, "a", encoding="utf-8") as f:
            f.write(line + "\n")
    except OSError:
        pass


# ----------------------------- cihaz bulma ----------------------------------
def _is_smalltv(base, timeout=1.5):
    try:
        with urllib.request.urlopen(base + "/health", timeout=timeout) as r:
            d = json.loads(r.read().decode())
            return "cl_online" in d and "pc_online" in d
    except Exception:
        return False

def _local_subnets():
    nets = set()
    try:
        for addrs in psutil.net_if_addrs().values():
            for a in addrs:
                if a.family == socket.AF_INET and a.netmask and not a.address.startswith(("127.", "169.254.")):
                    n = ipaddress.ip_network(f"{a.address}/{a.netmask}", strict=False)
                    if n.num_addresses <= 1024:
                        nets.add(n)
    except Exception:
        pass
    return nets

def find_device():
    cands = []
    try:
        cands.append(open(CACHE_PATH, encoding="utf-8").read().strip())
    except OSError:
        pass
    cands += [DEVICE_HINT, "http://smalltv.local"]
    for c in dict.fromkeys(x for x in cands if x):
        if _is_smalltv(c):
            return c
    hosts = [f"http://{h}" for n in _local_subnets() for h in n.hosts()]
    with concurrent.futures.ThreadPoolExecutor(64) as ex:
        for base, ok in zip(hosts, ex.map(lambda b: _is_smalltv(b, 0.8), hosts)):
            if ok:
                return base
    return None

def get_device(force=False):
    global device
    with _dev_lock:
        if device and not force:
            return device
        d = find_device()
        if d and d != device:
            log(f"cihaz bulundu: {d}")
            try:
                open(CACHE_PATH, "w", encoding="utf-8").write(d)
            except OSError:
                pass
        device = d
        return device

def post(path, payload):
    base = get_device()
    if not base:
        raise RuntimeError("cihaz bulunamadi")
    data = json.dumps(payload).encode()
    req = urllib.request.Request(base + path, data=data,
                                 headers={"Content-Type": "application/json"}, method="POST")
    try:
        with urllib.request.urlopen(req, timeout=3) as r:
            return r.status
    except (urllib.error.URLError, OSError):
        get_device(force=True)                      # IP degismis olabilir
        raise


# ------------------------------- PC -----------------------------------------
def collect_pc():
    cpu = psutil.cpu_percent(interval=None)
    vm = psutil.virtual_memory()
    d = {"host": HOST, "cpu": round(cpu, 1), "ram": round(vm.percent, 1),
         "ram_used": round(vm.used / 1024 ** 3, 1), "ram_total": round(vm.total / 1024 ** 3, 1)}
    try:
        f = psutil.cpu_freq()
        if f and f.current:
            d["cpu_ghz"] = round(f.current / 1000.0, 1)
    except Exception:
        pass
    return d


# ----------------------------- Claude ---------------------------------------
class NeedLogin(Exception):
    pass

def _load_oauth():
    try:
        with open(CRED_PATH, encoding="utf-8") as f:
            cred = json.load(f)
    except (OSError, ValueError):
        raise NeedLogin("Claude girisi gerekli")
    return cred, cred.get("claudeAiOauth") or {}

class _FileLock:
    def __enter__(self):
        t0 = time.time()
        while True:
            try:
                self.fd = os.open(LOCK_PATH, os.O_CREAT | os.O_EXCL | os.O_WRONLY); return self
            except FileExistsError:
                try:
                    if time.time() - os.path.getmtime(LOCK_PATH) > 60:
                        os.remove(LOCK_PATH); continue
                except OSError:
                    pass
                if time.time() - t0 > 30:
                    raise RuntimeError("kilit alinamadi")
                time.sleep(0.5)
    def __exit__(self, *a):
        os.close(self.fd)
        try: os.remove(LOCK_PATH)
        except OSError: pass

def _refresh(stale_token):
    """Token'i yeniler. Kilit altinda dosyayi tekrar okur: baskasi yenilediyse onu kullanir."""
    with _FileLock():
        cred, o = _load_oauth()
        tok = o.get("accessToken") or ""
        if tok and tok != stale_token and o.get("expiresAt", 0) > time.time() * 1000 + 60_000:
            return tok
        rt = o.get("refreshToken") or ""
        if not rt:
            raise NeedLogin("Claude girisi gerekli")
        body = json.dumps({"grant_type": "refresh_token", "refresh_token": rt, "client_id": CLIENT_ID}).encode()
        last = None
        for url in TOKEN_URLS:
            try:
                req = urllib.request.Request(url, data=body, method="POST",
                                             headers={"Content-Type": "application/json", "User-Agent": UA})
                tr = json.loads(urllib.request.urlopen(req, timeout=20).read().decode())
                break
            except urllib.error.HTTPError as e:
                last = e
                if e.code in (400, 401):                 # refresh token gecersiz -> giris sart
                    raise NeedLogin("Claude girisi gerekli")
            except Exception as e:
                last = e
        else:
            raise RuntimeError(f"token yenilenemedi: {last}")
        o["accessToken"] = tr["access_token"]
        o["refreshToken"] = tr.get("refresh_token", rt)
        o["expiresAt"] = int((time.time() + tr.get("expires_in", 28800)) * 1000)
        cred["claudeAiOauth"] = o
        tmp = CRED_PATH + ".smalltv.tmp"
        with open(tmp, "w", encoding="utf-8") as f:
            json.dump(cred, f)
        os.replace(tmp, CRED_PATH)
        log("token yenilendi")
        return o["accessToken"]

def _usage(token):
    req = urllib.request.Request(USAGE_URL, headers={
        "Authorization": f"Bearer {token}", "User-Agent": UA,
        "anthropic-beta": "oauth-2025-04-20", "Content-Type": "application/json"})
    return json.loads(urllib.request.urlopen(req, timeout=12).read().decode())

def collect_claude():
    _, o = _load_oauth()
    token = o.get("accessToken") or ""
    if not token or o.get("expiresAt", 0) < time.time() * 1000:
        token = _refresh(token)
    try:
        u = _usage(token)
    except urllib.error.HTTPError as e:
        if e.code != 401:
            raise
        u = _usage(_refresh(token))
    fh = u.get("five_hour") or {}
    sd = u.get("seven_day") or {}
    def used(x):
        return max(0, min(100, round(x.get("utilization") or 0)))
    def rst(x):
        s = x.get("resets_at")
        try:
            return int(datetime.fromisoformat(s).timestamp()) if s else 0
        except Exception:
            return 0
    return {"h5": used(fh), "d7": used(sd), "h5_reset": rst(fh), "d7_reset": rst(sd), "err": ""}


def claude_loop():
    last_err, last_val = None, None
    while True:
        try:
            c = collect_claude()
            post("/claude", c)
            if (c["h5"], c["d7"]) != last_val:
                log(f"[claude] 5 saat %{c['h5']}  haftalik %{c['d7']} kullanildi")
            last_err, last_val = None, (c["h5"], c["d7"])
        except NeedLogin as e:
            msg = str(e)
            if msg != last_err:
                log(f"[claude] {msg} - terminalde `claude` calistirip /login yap")
            last_err = msg
            try: post("/claude", {"err": msg})
            except Exception: pass
        except Exception as e:
            msg = str(e)[:120]
            if msg != last_err:
                log(f"[claude] hata: {msg}")
            last_err = msg
        time.sleep(CLAUDE_INTERVAL)


def main():
    try:
        _single = socket.socket(); _single.bind(("127.0.0.1", SINGLE_PORT))
    except OSError:
        log("ajan zaten calisiyor, cikiliyor"); return
    log(f"SmallTV ajani v3 basladi (PC {PC_INTERVAL:g}s, Claude {CLAUDE_INTERVAL:g}s) host={HOST}")
    if not get_device():
        log("cihaz bulunamadi, aramaya devam edilecek")
    psutil.cpu_percent(interval=None)
    if ENABLE_CLAUDE:
        threading.Thread(target=claude_loop, daemon=True).start()
    fails = 0
    while True:
        try:
            post("/stats", collect_pc()); fails = 0
        except Exception as e:
            fails += 1
            if fails in (1, 30) or fails % 300 == 0:
                log(f"[pc] gonderilemedi ({e})")
            if fails > 3:
                time.sleep(10)                         # cihaz kapaliyken agi bogma
        time.sleep(PC_INTERVAL)


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        pass
