"""SportLineBot dashboard server.

Flask + Socket.IO web app. It keeps a WebSocket connection to the ESP32-S3
robot (port 81), forwards commands, and runs the NLP and anomaly models.

Run:  python server/server.py   then open http://127.0.0.1:5000
"""

import os
import secrets
import threading
import time

import websockets.sync.client
from flask import Flask, jsonify, render_template
from flask_socketio import SocketIO, emit

from ml_anomaly import detect_anomaly
from nlp_robot import predict_intent

ESP32_PORT = 81
STATUS_INTERVAL_S = 5
# The robot has no speed sensor; the anomaly model gets a nominal value.
NOMINAL_SPEED_PERCENT = 90

app = Flask(__name__, static_folder="static")
app.config["SECRET_KEY"] = os.getenv("SECRET_KEY") or secrets.token_hex(16)
socketio = SocketIO(app, cors_allowed_origins="*", async_mode="threading")

esp32_ws = None
esp32_ip = None
target_ip = None
connected = False
tracing = False
_connection_thread_started = False


# --- ESP32 connection --------------------------------------------------------

def start_connection_thread():
    """Keep one background thread that (re)connects to the current target IP."""
    global _connection_thread_started
    if _connection_thread_started:
        return
    _connection_thread_started = True
    threading.Thread(target=_connection_loop, daemon=True).start()


def _connection_loop():
    global esp32_ws, connected, esp32_ip
    retry_delay = 2

    while True:
        ip = target_ip
        if not ip:
            time.sleep(1)
            continue

        uri = f"ws://{ip}:{ESP32_PORT}"
        try:
            print(f"Connecting to {uri}")
            with websockets.sync.client.connect(
                uri, ping_interval=30, ping_timeout=60,
                close_timeout=5, open_timeout=10,
            ) as ws:
                esp32_ws = ws
                connected = True
                esp32_ip = ip
                retry_delay = 2
                print(f"ESP32 connected: {uri}")
                socketio.emit("connection_status", {"success": True, "ip": ip})

                for message in ws:
                    handle_esp32_message(str(message))
                    if ip != target_ip:   # user switched to another robot
                        break

        except Exception as exc:
            connected = False
            esp32_ws = None
            print(f"Connection lost: {exc}")
            socketio.emit("connection_status", {"success": False, "error": str(exc)})
            time.sleep(retry_delay)
            retry_delay = min(retry_delay * 2, 30)


def send_command(cmd):
    """Send one command line to the robot. Returns True on success."""
    global esp32_ws, connected

    if not connected or esp32_ws is None:
        socketio.emit("error_msg", {"msg": "ESP32 not connected"})
        return False
    try:
        esp32_ws.send(cmd + "\n")
        print(f"-> ESP32: {cmd}")
        return True
    except Exception as exc:
        print(f"Send error: {exc}")
        connected = False
        esp32_ws = None
        socketio.emit("error_msg", {"msg": f"ESP32 disconnected: {exc}"})
        socketio.emit("connection_status", {"success": False, "error": str(exc)})
        return False


def handle_esp32_message(msg):
    """Parse a message from the robot and notify the dashboard."""
    global tracing

    print(f"<- ESP32: {msg}")
    socketio.emit("esp32_reply", {"data": msg})

    if msg.startswith("STATUS:"):
        parts = msg.split(":")
        if len(parts) == 3:
            try:
                paint = float(parts[1])
                distance = float(parts[2])
            except ValueError:
                return
            label, confidence = detect_anomaly(distance, paint, NOMINAL_SPEED_PERCENT)
            socketio.emit("anomaly", {
                "label": label, "confidence": confidence,
                "paint": paint, "distance": distance,
            })
            socketio.emit("led_status", {"green": label == "NORMAL", "red": label != "NORMAL"})

    elif msg.startswith("ALERT:"):
        alert_type = msg.split(":")[1]
        socketio.emit("alert", {"type": alert_type})
        if alert_type in ("OBSTACLE", "PAINT_EMPTY", "PAINT_LOW"):
            socketio.emit("led_status", {"green": False, "red": True})

    elif msg.startswith("STEP:"):
        parts = msg.split(":", 2)
        socketio.emit("trace_step", {
            "number": parts[1],
            "name": parts[2] if len(parts) > 2 else "",
        })

    elif msg.startswith("BEGIN:"):
        tracing = True
        socketio.emit("trace_started", {})
        socketio.emit("led_status", {"green": True, "red": False})

    elif msg.startswith("DONE:"):
        tracing = False
        socketio.emit("trace_finished", {})
        socketio.emit("led_status", {"green": True, "red": False})


def status_poller():
    """Ask the robot for its status periodically while it is idle."""
    while True:
        time.sleep(STATUS_INTERVAL_S)
        if connected and esp32_ws is not None and not tracing:
            send_command("STATUS")


threading.Thread(target=status_poller, daemon=True).start()


# --- HTTP routes -------------------------------------------------------------

@app.route("/")
def index():
    return render_template("index.html")


@app.route("/status")
def status():
    return jsonify({"connected": connected, "ip": esp32_ip or "", "tracing": tracing})


# --- Socket.IO events ----------------------------------------------------------

@socketio.on("connect_esp32")
def on_connect_esp32(data):
    global target_ip
    ip = data.get("ip", "").strip()
    if not ip:
        emit("error_msg", {"msg": "Missing IP address"})
        return
    target_ip = ip
    start_connection_thread()


@socketio.on("nlp_command")
def on_nlp_command(data):
    text = data.get("text", "").strip()
    if not text:
        return

    intent, confidence = predict_intent(text)
    emit("nlp_result", {"intent": intent, "confidence": confidence, "text": text})

    if intent == "football":
        send_command("FOOTBALL_FIELD")
    elif intent == "stop":
        send_command("STOP")
    elif intent == "pause":
        send_command("PAUSE")
    elif intent == "resume":
        send_command("RESUME")
    elif intent in ("paint_level", "status"):
        send_command("STATUS")
    elif intent == "pump_test":
        send_command("PUMP_TEST")
    elif intent == "scan":
        send_command("SCAN")


@socketio.on("direct_command")
def on_direct_command(data):
    cmd = data.get("cmd", "").strip()
    if cmd:
        ok = send_command(cmd)
        emit("command_sent", {"cmd": cmd, "success": ok})


@socketio.on("emergency_stop")
def on_emergency_stop(data):
    send_command("STOP")
    emit("command_sent", {"cmd": "STOP", "success": True})


@socketio.on("pause")
def on_pause(data):
    send_command("PAUSE")


@socketio.on("resume")
def on_resume(data):
    send_command("RESUME")


if __name__ == "__main__":
    host = os.getenv("HOST", "127.0.0.1")
    port = int(os.getenv("PORT", "5000"))
    print(f"SportLineBot dashboard: http://{host}:{port}")
    socketio.run(app, host=host, port=port, debug=False, allow_unsafe_werkzeug=True)
