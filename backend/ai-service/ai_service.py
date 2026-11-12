"""
HEAT STRESS GUARDIAN - Cloud AI explained alerts (GitHub Actions)

Each time GitHub runs this program (every 5 minutes), it:
  1. Signs in to Firebase with the device login.
  2. Reads the latest reading and the last 10 minutes of history.
  3. Works out the trend (heat index rising or falling, average heart rate).
  4. Asks an AI model for a short explanation in English,
     but only when the status changed or the last message is old,
     to stay within the free AI limits.
  5. Writes the result to Firebase at /HeatStressGuardian/ai/latest.

AI provider:
  - Gemini (free tier) if GEMINI_API_KEY is set
  - Claude if ANTHROPIC_API_KEY is set instead
  - Otherwise a fixed safety message is written, so there is always a message.

Run it:
  python ai_service.py --demo    test with sample data, no Firebase needed
  python ai_service.py --once    one cloud run (what GitHub Actions uses)
"""

import argparse
import json
import os
import sys
import time
from pathlib import Path

import requests

try:
    from dotenv import load_dotenv
    load_dotenv(Path(__file__).resolve().parent / "ai_settings.env")
except ImportError:
    pass  # on GitHub the values come from repository secrets instead

# ---------------- SETTINGS ----------------

FIREBASE_API_KEY = os.getenv("FIREBASE_API_KEY", "")
FIREBASE_EMAIL = os.getenv("FIREBASE_EMAIL", "")
FIREBASE_PASSWORD = os.getenv("FIREBASE_PASSWORD", "")
DATABASE_URL = os.getenv("DATABASE_URL", "").strip().rstrip("/")

GEMINI_API_KEY = os.getenv("GEMINI_API_KEY", "")
GEMINI_MODEL = os.getenv("GEMINI_MODEL", "") or "gemini-2.5-flash"
ANTHROPIC_API_KEY = os.getenv("ANTHROPIC_API_KEY", "")
CLAUDE_MODEL = os.getenv("CLAUDE_MODEL", "") or "claude-haiku-5-5"

BASE_PATH = "/HeatStressGuardian"
HISTORY_RECORDS = 20                 # 20 records x 30 s = last 10 minutes
STALE_SECONDS = 120                  # no reading for 2 minutes = device offline
REFRESH_ALERT_MINUTES = 10           # refresh the message during CAUTION/DANGER
REFRESH_SAFE_MINUTES = 60            # refresh the message during SAFE

LEVEL_NAMES = {0: "SAFE", 1: "CAUTION", 2: "DANGER"}

FALLBACK = {
    0: "Conditions are safe. Keep drinking water regularly.",
    1: "Heat is building up. Drink water and take a short break in the shade.",
    2: "Danger level heat stress. Stop work, move to shade, drink water and tell the supervisor now.",
}

SYSTEM_PROMPT = """You write short safety messages for outdoor and factory workers \
wearing a heat-stress monitor. You receive sensor facts as JSON.

Rules:
- Use only the facts given. Do not invent numbers.
- The risk level is already decided by the device. Never lower it or contradict it.
- SAFE: one calm reassurance. CAUTION: tell the worker to drink water and rest in shade.
  DANGER: tell the worker to stop work, move to shade, drink water and inform the supervisor.
- Plain words a worker with little schooling can follow. No medical terms, no diagnosis.
- Each message is at most 2 short sentences.
- "reasons" lists the 1 to 3 facts that explain the level, each under 12 words.

- Write in simple English.

Reply with JSON only, in exactly this shape:
{"message": "...", "reasons": ["...", "..."]}"""


# ---------------- FIREBASE ----------------

class Firebase:

    def __init__(self):
        self.token = None

    def sign_in(self):
        url = ("https://identitytoolkit.googleapis.com/v1/accounts:signInWithPassword"
               "?key=" + FIREBASE_API_KEY)
        r = requests.post(url, json={"email": FIREBASE_EMAIL, "password": FIREBASE_PASSWORD,
                                     "returnSecureToken": True}, timeout=20)
        if r.status_code != 200:
            reason = r.json().get("error", {}).get("message", r.text)
            raise RuntimeError("Firebase sign-in failed: " + str(reason))
        self.token = r.json()["idToken"]

    def get(self, path, extra=None):
        params = {"auth": self.token}
        if extra:
            params.update(extra)
        r = requests.get(DATABASE_URL + path + ".json", params=params, timeout=20)
        r.raise_for_status()
        return r.json()

    def put(self, path, data):
        r = requests.put(DATABASE_URL + path + ".json", params={"auth": self.token},
                         json=data, timeout=20)
        r.raise_for_status()

    def post(self, path, data):
        r = requests.post(DATABASE_URL + path + ".json", params={"auth": self.token},
                          json=data, timeout=20)
        r.raise_for_status()


# ---------------- TREND ----------------

def is_number(value):
    return isinstance(value, (int, float)) and not isinstance(value, bool)


def build_facts(latest, history):
    level = int(latest.get("riskLevel", 0) or 0)

    facts = {
        "risk_level": LEVEL_NAMES.get(level, "SAFE"),
        "air_temperature_c": latest.get("temperature"),
        "humidity_percent": latest.get("humidity"),
        "heat_index_c": latest.get("heatIndex"),
        "skin_or_surface_ir_temperature_c": latest.get("irObjectTemp"),
        "heart_rate_bpm": latest.get("heartRate") if latest.get("fingerDetected") else None,
    }

    records = [h for h in history if isinstance(h, dict)]
    heat = [h["heatIndex"] for h in records if is_number(h.get("heatIndex"))]
    hearts = [h["heartRate"] for h in records
              if h.get("fingerDetected") and is_number(h.get("heartRate")) and h["heartRate"] > 0]
    stamps = [h["timestamp"] for h in records if is_number(h.get("timestamp"))]

    if len(heat) >= 2:
        change = round(heat[-1] - heat[0], 1)
        facts["heat_index_change_c"] = change
        facts["heat_trend"] = "rising" if change >= 1.0 else "falling" if change <= -1.0 else "steady"

    if len(stamps) >= 2:
        facts["trend_window_minutes"] = round((max(stamps) - min(stamps)) / 60000, 1)

    if hearts:
        facts["average_heart_rate_bpm"] = round(sum(hearts) / len(hearts))

    if records:
        in_alert = sum(1 for h in records if int(h.get("riskLevel", 0) or 0) >= 1)
        facts["share_of_window_in_alert_percent"] = round(100 * in_alert / len(records))

    return level, {k: v for k, v in facts.items() if v is not None}


# ---------------- AI ----------------

def extract_json(text):
    start, end = text.find("{"), text.rfind("}")
    if start == -1 or end == -1:
        raise ValueError("no JSON in AI reply")
    return json.loads(text[start:end + 1])


def ask_gemini(facts):
    url = f"https://generativelanguage.googleapis.com/v1beta/models/{GEMINI_MODEL}:generateContent"
    body = {
        "systemInstruction": {"parts": [{"text": SYSTEM_PROMPT}]},
        "contents": [{"role": "user", "parts": [{"text": json.dumps(facts, ensure_ascii=False)}]}],
        "generationConfig": {"responseMimeType": "application/json", "temperature": 0.3},
    }
    r = requests.post(url, headers={"x-goog-api-key": GEMINI_API_KEY}, json=body, timeout=60)
    if r.status_code != 200:
        raise RuntimeError(f"Gemini error {r.status_code}: {r.text[:300]}")
    parts = r.json()["candidates"][0]["content"]["parts"]
    return extract_json("".join(p.get("text", "") for p in parts))


def ask_claude(facts):
    r = requests.post(
        "https://api.anthropic.com/v1/messages",
        headers={"x-api-key": ANTHROPIC_API_KEY, "anthropic-version": "2023-06-01",
                 "content-type": "application/json"},
        json={"model": CLAUDE_MODEL, "max_tokens": 500, "system": SYSTEM_PROMPT,
              "messages": [{"role": "user", "content": json.dumps(facts, ensure_ascii=False)}]},
        timeout=60)
    if r.status_code != 200:
        raise RuntimeError(f"Claude error {r.status_code}: {r.text[:300]}")
    text = "".join(b.get("text", "") for b in r.json()["content"] if b.get("type") == "text")
    return extract_json(text)


def ask_ai(level, facts):
    """Returns (result, source, model). Falls back to a fixed message if the AI call fails."""
    fallback = {"message": FALLBACK[level], "reasons": []}

    if GEMINI_API_KEY:
        provider, model, call = "gemini", GEMINI_MODEL, ask_gemini
    elif ANTHROPIC_API_KEY:
        provider, model, call = "claude", CLAUDE_MODEL, ask_claude
    else:
        return fallback, "fallback (no AI key set)", ""

    try:
        data = call(facts)
        result = {
            "message": str(data.get("message", "")).strip() or FALLBACK[level],
            "reasons": [str(x) for x in data.get("reasons", [])][:3],
        }
        return result, provider, model
    except Exception as error:
        print("  AI call failed, using the fixed message. Reason:", error)
        return fallback, "fallback (AI error)", model


def make_record(level, facts, result, source, model):
    return {
        "riskLevel": level,
        "status": LEVEL_NAMES.get(level, "SAFE"),
        "message": result["message"],
        "reasons": result["reasons"],
        "facts": facts,
        "source": source,
        "model": model,
        "generatedAt": {".sv": "timestamp"},
    }


def show(record):
    print("  Status :", record["status"])
    print("  Message:", record["message"])
    for reason in record["reasons"]:
        print("   -", reason)
    print("  Source :", record["source"], record["model"])


# ---------------- DEMO ----------------

def run_demo():
    now = int(time.time() * 1000)

    def series(a, b, heart, level):
        return [{"heatIndex": round(a + (b - a) * i / 9, 1), "heartRate": heart,
                 "fingerDetected": True, "riskLevel": level,
                 "timestamp": now - (9 - i) * 30000} for i in range(10)]

    cases = [
        ("Cool morning", {"temperature": 27.0, "humidity": 55.0, "heatIndex": 28.1,
                          "heartRate": 78, "fingerDetected": True, "riskLevel": 0}, series(27.8, 28.1, 78, 0)),
        ("Hot afternoon", {"temperature": 33.0, "humidity": 68.0, "heatIndex": 36.4,
                           "heartRate": 104, "fingerDetected": True, "riskLevel": 1}, series(33.9, 36.4, 100, 1)),
        ("Very hot, heart racing", {"temperature": 36.0, "humidity": 72.0, "heatIndex": 43.2,
                                    "heartRate": 132, "fingerDetected": True, "riskLevel": 2}, series(40.1, 43.2, 128, 2)),
    ]
    for title, latest, history in cases:
        print("\n===", title, "===")
        level, facts = build_facts(latest, history)
        result, source, model = ask_ai(level, facts)
        show(make_record(level, facts, result, source, model))
    print("\nDemo finished. Nothing was written to Firebase.")


# ---------------- ONE CLOUD RUN ----------------

def check_settings():
    missing = [name for name, value in [
        ("FIREBASE_API_KEY", FIREBASE_API_KEY), ("FIREBASE_EMAIL", FIREBASE_EMAIL),
        ("FIREBASE_PASSWORD", FIREBASE_PASSWORD), ("DATABASE_URL", DATABASE_URL),
    ] if not value or value.startswith("YOUR")]
    if missing:
        print("Missing settings:", ", ".join(missing))
        sys.exit(1)
    if not (GEMINI_API_KEY or ANTHROPIC_API_KEY):
        print("Note: no AI key set, so a fixed safety message will be written.")


def run_once():
    check_settings()
    fb = Firebase()
    fb.sign_in()
    print("Signed in to Firebase.")

    latest = fb.get(BASE_PATH + "/latest")
    if not isinstance(latest, dict):
        print("No readings in Firebase yet. Nothing to do.")
        return

    previous = fb.get(BASE_PATH + "/ai/latest") or {}
    now_ms = time.time() * 1000

    stamp = latest.get("timestamp")
    age_s = (now_ms - stamp) / 1000 if is_number(stamp) else 0

    if age_s > STALE_SECONDS:
        print(f"Device offline: last reading {round(age_s)} seconds ago.")
        if previous.get("status") != "OFFLINE":
            fb.put(BASE_PATH + "/ai/latest", {
                "status": "OFFLINE",
                "message": "No new readings from the device. Check power and Wi-Fi.",
                "reasons": [], "source": "service",
                "generatedAt": {".sv": "timestamp"},
            })
        return

    level = int(latest.get("riskLevel", 0) or 0)
    status_changed = previous.get("status") != LEVEL_NAMES.get(level)
    prev_time = previous.get("generatedAt")
    minutes_old = (now_ms - prev_time) / 60000 if is_number(prev_time) else 9999
    refresh_after = REFRESH_ALERT_MINUTES if level >= 1 else REFRESH_SAFE_MINUTES

    if not status_changed and minutes_old < refresh_after:
        print(f"Status still {LEVEL_NAMES[level]} and message is {round(minutes_old)} min old. No AI call needed.")
        return

    raw = fb.get(BASE_PATH + "/history", {"orderBy": '"$key"', "limitToLast": HISTORY_RECORDS})
    history = list(raw.values()) if isinstance(raw, dict) else []

    level, facts = build_facts(latest, history)
    result, source, model = ask_ai(level, facts)
    record = make_record(level, facts, result, source, model)

    fb.put(BASE_PATH + "/ai/latest", record)
    if status_changed:
        fb.post(BASE_PATH + "/ai/history", record)

    print("New AI explanation written to Firebase.")
    show(record)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Heat Stress Guardian cloud AI alerts")
    parser.add_argument("--demo", action="store_true", help="test with sample data")
    parser.add_argument("--once", action="store_true", help="one cloud run")
    args = parser.parse_args()
    if args.demo:
        run_demo()
    else:
        run_once()
