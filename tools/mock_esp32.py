"""Fake ESP32 for trying the dashboard without hardware.

Speaks the same text protocol as firmware/robot_code on ws://0.0.0.0:81 and
simulates the football-field steps. Then connect the dashboard to 127.0.0.1.

Run:  python tools/mock_esp32.py
"""

import random
import time

from websockets.sync.server import serve

STEPS = [
    "Outer rectangle", "Halfway line", "Centre circle", "Left penalty area",
    "Left goal area", "Right penalty area", "Right goal area",
]
STEP_SECONDS = 1.5

state = {"paused": False, "paint": 80}


def handle(ws):
    ws.send("READY")
    for raw in ws:
        cmd = raw.strip()
        print("CMD:", cmd)

        if cmd == "STATUS":
            ws.send(f"STATUS:{state['paint']}:{random.choice([999, 120, 80, 45])}")
        elif cmd == "FOOTBALL_FIELD":
            ws.send("BEGIN:FOOTBALL_FIELD")
            for i, name in enumerate(STEPS, start=1):
                ws.send(f"STEP:{i}:{name}")
                time.sleep(STEP_SECONDS)
                state["paint"] = max(0, state["paint"] - 3)
            ws.send("DONE:FOOTBALL_FIELD")
        elif cmd in ("STOP", "PAUSE", "RESUME", "PUMP_TEST"):
            ws.send(f"OK:{cmd}")
        elif cmd == "SCAN":
            ws.send("SCAN:0=120,30=95,60=300,90=180,120=75,150=250,180=130,")
        elif cmd == "IP":
            ws.send("IP:127.0.0.1")
        else:
            ws.send(f"ERR:UNKNOWN_COMMAND:{cmd}")


if __name__ == "__main__":
    print("Mock ESP32 listening on ws://0.0.0.0:81")
    with serve(handle, "0.0.0.0", 81) as server:
        server.serve_forever()
