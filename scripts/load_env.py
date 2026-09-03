from pathlib import Path
import re

Import("env")

required_names = ("WIFI_SSID", "WIFI_PASSWORD", "WEBUI_USERNAME", "WEBUI_PASSWORD")
env_file = Path(env.subst("$PROJECT_DIR")) / ".env.local"

if not env_file.is_file():
    raise RuntimeError("Missing .env.local with WiFi and Web UI credentials.")

values = {}
for line in env_file.read_text(encoding="utf-8").splitlines():
    match = re.match(r"^\s*([A-Z0-9_]+)\s*=\s*[\"']?(.*?)[\"']?\s*$", line)
    if match:
        values[match.group(1)] = match.group(2)

missing_names = [name for name in required_names if not values.get(name)]
if missing_names:
    raise RuntimeError("Missing values in .env.local: " + ", ".join(missing_names))

def as_cpp_string(value):
    escaped = value.replace("\\", "\\\\").replace('"', '\\"')
    return '\\"' + escaped + '\\"'

env.Append(CPPDEFINES=[(name, as_cpp_string(values[name])) for name in required_names])