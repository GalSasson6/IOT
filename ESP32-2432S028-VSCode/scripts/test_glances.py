import json
import os
import urllib.request
from pathlib import Path


def load_env(path: Path) -> None:
    if not path.exists():
        return
    for raw_line in path.read_text(encoding="utf-8").splitlines():
        line = raw_line.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        key, value = line.split("=", 1)
        os.environ.setdefault(key.strip(), value.strip().strip("\"'"))


load_env(Path(__file__).resolve().parents[1] / ".env")
glances_host = os.environ.get("GLANCES_HOST", "192.168.1.100")
glances_port = os.environ.get("GLANCES_PORT", "61208")
base_url = f"http://{glances_host}:{glances_port}/api/3"
endpoints = ["/quicklook", "/fs", "/sensors"]

for ep in endpoints:
    try:
        req = urllib.request.Request(base_url + ep)
        with urllib.request.urlopen(req, timeout=3) as response:
            data = json.loads(response.read().decode())
            print(f"--- {ep} ---")
            print(json.dumps(data, indent=2)[:500] + "...\n")
    except Exception as e:
        print(f"Failed {ep}: {e}")
