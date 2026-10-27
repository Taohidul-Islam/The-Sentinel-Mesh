"""
Phyxon Sentinel Mesh v2 - Laptop Server
=========================================
Polls OpenWeatherMap every 10 minutes,
detects flood/cyclone warnings in the forecast, serves a simple
/alerts endpoint that the receiver ESP32 polls every 60 seconds.

Also serves a dashboard at http://localhost:8080

Setup:
    pip install flask requests

Run:
    python3 app.py

Config (edit the lines below before running):
    CITY          — city name for weather lookup
    OWM_API_KEY   — your OpenWeatherMap free API key
    PORT          — 8080 (matches receiver firmware)
"""

from flask import Flask, jsonify, render_template_string
import requests
import threading
import time
from datetime import datetime

# ============================================================
# CONFIG — edit these before running
# ============================================================
CITY        = "##"          # change to your target city
OWM_API_KEY = "##" # paste your OpenWeatherMap key
PORT        = 8080
POLL_EVERY  = 600  # seconds (10 minutes)

# ============================================================
# STATE
# ============================================================
weather_state = {
    "last_updated": None,
    "city": CITY,
    "current": {},
    "forecast": [],
    "flood_alert": "NONE",
    "cyclone_alert": "NONE",
    "summary": "No data yet",
    "raw_alerts": [],
    "error": None,
}

app = Flask(__name__)

# ============================================================
# OPENWEATHERMAP POLLING
# ============================================================
CYCLONE_KEYWORDS = [
    "tropical storm", "cyclone", "hurricane", "typhoon",
    "severe storm", "violent storm"
]
FLOOD_KEYWORDS = [
    "flood", "flash flood", "heavy rain", "extreme rain",
    "torrential", "inundation", "storm surge"
]

def fetch_weather():
    """Poll OWM current weather + 5-day forecast + alerts."""
    global weather_state
    try:
        base = "https://api.openweathermap.org/data/2.5"

        # current weather
        cur_r = requests.get(
            f"{base}/weather",
            params={"q": CITY, "appid": OWM_API_KEY, "units": "metric"},
            timeout=10
        )
        cur_r.raise_for_status()
        cur = cur_r.json()

        # 5-day / 3-hour forecast
        fc_r = requests.get(
            f"{base}/forecast",
            params={"q": CITY, "appid": OWM_API_KEY, "units": "metric"},
            timeout=10
        )
        fc_r.raise_for_status()
        fc = fc_r.json()

        # OWM One Call alerts (free tier may not include this, handled gracefully)
        alerts = []
        try:
            lat = cur["coord"]["lat"]
            lon = cur["coord"]["lon"]
            oc_r = requests.get(
                "https://api.openweathermap.org/data/3.0/onecall",
                params={
                    "lat": lat, "lon": lon,
                    "appid": OWM_API_KEY,
                    "exclude": "minutely,hourly,daily",
                    "units": "metric"
                },
                timeout=10
            )
            if oc_r.status_code == 200:
                oc = oc_r.json()
                alerts = oc.get("alerts", [])
        except Exception:
            pass  # One Call may not be on free tier

        # --- detect flood/cyclone in alerts + forecast descriptions ---
        flood_msg   = "NONE"
        cyclone_msg = "NONE"

        # check official alerts first
        for a in alerts:
            desc  = (a.get("description", "") + " " + a.get("event", "")).lower()
            event = a.get("event", "Unknown Alert")
            for kw in FLOOD_KEYWORDS:
                if kw in desc:
                    flood_msg = event
                    break
            for kw in CYCLONE_KEYWORDS:
                if kw in desc:
                    cyclone_msg = event
                    break

        # check 24h forecast descriptions as a fallback
        if flood_msg == "NONE" or cyclone_msg == "NONE":
            now_ts = time.time()
            for entry in fc.get("list", []):
                if entry["dt"] > now_ts + 86400:  # only next 24h
                    break
                desc = entry.get("weather", [{}])[0].get("description", "").lower()
                main = entry.get("weather", [{}])[0].get("main", "").lower()
                full = desc + " " + main
                if flood_msg == "NONE":
                    for kw in FLOOD_KEYWORDS:
                        if kw in full:
                            flood_msg = f"Heavy rain / flood risk ({desc})"
                            break
                if cyclone_msg == "NONE":
                    for kw in CYCLONE_KEYWORDS:
                        if kw in full:
                            cyclone_msg = f"Severe storm / cyclone risk ({desc})"
                            break

        # current summary line
        summary = (
            f"{cur['weather'][0]['description'].title()}, "
            f"{cur['main']['temp']:.1f}C, "
            f"Hum {cur['main']['humidity']}%, "
            f"Pres {cur['main']['pressure']} hPa"
        )

        weather_state.update({
            "last_updated": datetime.now().strftime("%Y-%m-%d %H:%M:%S"),
            "city": CITY,
            "current": cur,
            "forecast": fc.get("list", [])[:8],  # next 24h (8 × 3h slots)
            "flood_alert": flood_msg,
            "cyclone_alert": cyclone_msg,
            "summary": summary,
            "raw_alerts": alerts,
            "error": None,
        })
        print(f"[OWM] Updated — flood={flood_msg} | cyclone={cyclone_msg}")

    except Exception as e:
        weather_state["error"] = str(e)
        weather_state["summary"] = f"Fetch error: {e}"
        print(f"[OWM] Error: {e}")


def poll_loop():
    """Background thread: poll OWM every POLL_EVERY seconds."""
    while True:
        fetch_weather()
        time.sleep(POLL_EVERY)


# ============================================================
# ENDPOINTS
# ============================================================
@app.route("/alerts")
def alerts():
    """
    Plain-text endpoint the receiver ESP32 polls.
    Format:
        flood:<message or NONE>
        cyclone:<message or NONE>
        summary:<one line>
    """
    s = weather_state
    body = (
        f"flood:{s['flood_alert']}\n"
        f"cyclone:{s['cyclone_alert']}\n"
        f"summary:{s['summary']}\n"
    )
    return body, 200, {"Content-Type": "text/plain"}


@app.route("/api/weather")
def api_weather():
    """JSON endpoint — optional, for debugging or future use."""
    s = weather_state
    return jsonify({
        "city":          s["city"],
        "last_updated":  s["last_updated"],
        "flood_alert":   s["flood_alert"],
        "cyclone_alert": s["cyclone_alert"],
        "summary":       s["summary"],
        "error":         s["error"],
    })


DASHBOARD_HTML = """
<!DOCTYPE html>
<html>
<head>
  <meta name="viewport" content="width=device-width,initial-scale=1">
  <meta http-equiv="refresh" content="60">
  <title>Phyxon Sentinel — Laptop Server</title>
  <style>
    body { font-family: sans-serif; margin: 20px; background: #111; color: #eee; }
    h2 { color: #5dcaa5; }
    .card { background: #1c1c1c; padding: 14px; border-radius: 8px; margin-bottom: 12px; }
    .alert { color: #f87171; font-weight: bold; }
    .ok    { color: #86efac; }
    .warn  { color: #fbbf24; }
    table  { border-collapse: collapse; width: 100%; }
    th, td { border: 1px solid #333; padding: 6px 10px; text-align: left; }
    th     { background: #222; }
  </style>
</head>
<body>
<h2>Phyxon Sentinel — Laptop Weather Server</h2>

<div class="card">
  <b>City:</b> {{ state.city }}<br>
  <b>Last updated:</b> {{ state.last_updated or 'Never' }}<br>
  <b>Summary:</b> {{ state.summary }}<br>
  {% if state.error %}<span class="warn">Last error: {{ state.error }}</span>{% endif %}
</div>

<div class="card">
  <b>Disaster alerts (next 24h)</b><br>
  Flood :
    {% if state.flood_alert != 'NONE' %}
      <span class="alert">{{ state.flood_alert }}</span>
    {% else %}
      <span class="ok">None</span>
    {% endif %}
  <br>
  Cyclone :
    {% if state.cyclone_alert != 'NONE' %}
      <span class="alert">{{ state.cyclone_alert }}</span>
    {% else %}
      <span class="ok">None</span>
    {% endif %}
</div>

{% if state.forecast %}
<div class="card">
  <b>Next 24h forecast</b>
  <table>
    <tr><th>Time</th><th>Desc</th><th>Temp °C</th><th>Hum %</th><th>Rain mm</th></tr>
    {% for f in state.forecast %}
    <tr>
      <td>{{ f.dt_txt }}</td>
      <td>{{ f.weather[0].description }}</td>
      <td>{{ f.main.temp | round(1) }}</td>
      <td>{{ f.main.humidity }}</td>
      <td>{{ f.get('rain', {}).get('3h', 0) }}</td>
    </tr>
    {% endfor %}
  </table>
</div>
{% endif %}

{% if state.raw_alerts %}
<div class="card">
  <b>Official OWM alerts</b><br>
  {% for a in state.raw_alerts %}
    <b>{{ a.event }}</b>: {{ a.description }}<br>
  {% endfor %}
</div>
{% endif %}

</body>
</html>
"""

@app.route("/")
def dashboard():
    return render_template_string(DASHBOARD_HTML, state=weather_state)


# ============================================================
# MAIN
# ============================================================
if __name__ == "__main__":
    print(f"[BOOT] Phyxon Sentinel Laptop Server")
    print(f"[BOOT] City: {CITY}")
    print(f"[BOOT] Fetching initial weather data...")
    fetch_weather()  # first fetch before starting server

    poller = threading.Thread(target=poll_loop, daemon=True)
    poller.start()

    print(f"[BOOT] Server starting on port {PORT}")
    print(f"[BOOT] Dashboard : http://localhost:{PORT}")
    print(f"[BOOT] Alerts EP : http://localhost:{PORT}/alerts  (receiver polls this)")
    app.run(host="0.0.0.0", port=PORT, debug=False)
