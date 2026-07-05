#!/usr/bin/env python3
# ============================================================================
#  SmallTV Ajani (v2) - PC istatistikleri + Claude kalan limit
#    PC (CPU/RAM)  -> POST /stats   her 2 sn
#    Claude limit  -> POST /claude  her 2 dk   (ayri thread, PC'yi bloklamaz)
#
#  Claude verisi: ~/.claude/.credentials.json'daki OAuth access token ile
#  https://api.anthropic.com/api/oauth/usage cagrilir. Token'a "yamanir";
#  yalnizca 401 (suresi dolmus) durumunda refresh_token ile yenilenir ve
#  dosyaya atomik geri yazilir (Claude Code ile cakismayi en aza indirmek icin).
# ============================================================================
import json, time, socket, sys, os, threading, urllib.request, urllib.error
from datetime import datetime

# ------------------------------ AYARLAR -------------------------------------
DEVICE          = "http://192.168.1.31"     # cihaz IP'si (kendi cihazininkiyle degistir)
PC_INTERVAL     = 2.0                        # PC verisi araligi (sn)
ENABLE_CLAUDE   = True                       # False -> Claude limiti HIC cekilmez, token'a DOKUNULMAZ
CLAUDE_INTERVAL = 300.0                      # Claude limiti araligi (sn)
# ----------------------------------------------------------------------------
#  >>> UYARI (Claude modulu) <<<
#  Bu bolum Anthropic'in BELGELENMEMIS bir ic ucunu (api/oauth/usage) senin
#  KENDI yerel OAuth token'inla cagirir. Salt-okunurdur; limitini/token hakkini
#  TUKETMEZ. Ama resmi bir API degildir - kendi hesabin, kendi riskin.
#  Rahatsizsan ENABLE_CLAUDE=False yap; PC istatistikleri yine calisir.
# ----------------------------------------------------------------------------
CRED_PATH = os.path.join(os.path.expanduser("~"), ".claude", ".credentials.json")
CLIENT_ID = "9d1c250a-e61b-44d9-88ed-5944d1962f5e"          # Claude Code OAuth client_id
TOKEN_URL = "https://api.anthropic.com/v1/oauth/token"
USAGE_URL = "https://api.anthropic.com/api/oauth/usage"

try:
    import psutil
except ImportError:
    import subprocess
    print("psutil kuruluyor..."); subprocess.run([sys.executable, "-m", "pip", "install", "--quiet", "psutil"]); import psutil

HOST = socket.gethostname()[:23]


def post(path, payload):
    data = json.dumps(payload).encode()
    req = urllib.request.Request(DEVICE + path, data=data,
                                 headers={"Content-Type": "application/json"}, method="POST")
    with urllib.request.urlopen(req, timeout=3) as r:
        return r.status


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
def _load_cred():
    with open(CRED_PATH, encoding="utf-8") as f:
        return json.load(f)

def _refresh_token():
    cred = _load_cred()
    o = cred["claudeAiOauth"]
    body = json.dumps({"grant_type": "refresh_token",
                       "refresh_token": o["refreshToken"], "client_id": CLIENT_ID}).encode()
    req = urllib.request.Request(TOKEN_URL, data=body,
                                 headers={"Content-Type": "application/json"}, method="POST")
    tr = json.loads(urllib.request.urlopen(req, timeout=20).read().decode())
    if not tr.get("access_token"):
        raise RuntimeError("refresh cevabinda access_token yok")
    o["accessToken"] = tr["access_token"]
    o["refreshToken"] = tr.get("refresh_token", o["refreshToken"])
    o["expiresAt"] = int((time.time() + tr.get("expires_in", 28800)) * 1000)
    cred["claudeAiOauth"] = o
    tmp = CRED_PATH + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(cred, f)
    os.replace(tmp, CRED_PATH)
    return o["accessToken"]

def _usage(token):
    req = urllib.request.Request(USAGE_URL, headers={
        "Authorization": f"Bearer {token}",
        "anthropic-beta": "oauth-2025-04-20", "Content-Type": "application/json"})
    return json.loads(urllib.request.urlopen(req, timeout=12).read().decode())

def collect_claude():
    token = _load_cred()["claudeAiOauth"]["accessToken"]
    try:
        u = _usage(token)
    except urllib.error.HTTPError as e:
        if e.code == 401:                 # token dolmus -> yenile
            u = _usage(_refresh_token())
        else:
            raise
    fh = u.get("five_hour") or {}
    sd = u.get("seven_day") or {}
    def used(x):                                  # KULLANILAN % (fotograftaki gibi)
        return max(0, min(100, round(x.get("utilization") or 0)))
    def rst(x):
        s = x.get("resets_at")
        try:
            return int(datetime.fromisoformat(s).timestamp()) if s else 0
        except Exception:
            return 0
    return {"h5": used(fh), "d7": used(sd), "h5_reset": rst(fh), "d7_reset": rst(sd)}


def claude_loop():
    while True:
        try:
            c = collect_claude()
            post("/claude", c)
            print(f"\n[claude] 5saat {c['h5']}%  7gun {c['d7']}% kullanildi")
        except Exception as e:
            print(f"\n[claude] hata: {str(e)[:90]}")
        time.sleep(CLAUDE_INTERVAL)


def main():
    print(f"SmallTV ajani -> {DEVICE}   (PC {PC_INTERVAL:g}s, Claude {CLAUDE_INTERVAL:g}s)   host={HOST}")
    psutil.cpu_percent(interval=None)                       # prime
    if ENABLE_CLAUDE:
        threading.Thread(target=claude_loop, daemon=True).start()
    else:
        print("Claude modulu KAPALI (ENABLE_CLAUDE=False) - sadece PC istatistikleri")
    while True:
        try:
            d = collect_pc()
            post("/stats", d)
            print(f"\rcpu {d['cpu']:5.1f}%  ram {d['ram']:5.1f}%  ({d['ram_used']}/{d['ram_total']} GB)   ", end="", flush=True)
        except Exception as e:
            print(f"\n[pc] gonderilemedi ({e})")
        time.sleep(PC_INTERVAL)


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print("\nbitti.")
