"""Anomaly detection for SportLineBot.

Hard safety rules run first (empty paint, close obstacle, low paint, stuck).
Anything else goes through an Isolation Forest trained on synthetic "normal" situations.

Features: [distance_cm, paint_percent, speed_percent]
Labels  : NORMAL, OBSTACLE_CLOSE, PAINT_EMPTY, PAINT_LOW, ROBOT_STUCK,
          GENERAL_ANOMALY
"""

import pickle
from pathlib import Path

import numpy as np
from sklearn.ensemble import IsolationForest

MODEL_FILE = Path(__file__).resolve().parent.parent / "models" / "ml_model.pkl"

# Critical thresholds
OBSTACLE_THRESHOLD_CM = 15   # obstacle closer than this: stop
PAINT_EMPTY_PCT = 3          # paint at or below this: pump stopped
PAINT_LOW_PCT = 10           # paint at or below this: warning
STUCK_SPEED_PCT = 20         # speed at or below this: robot stuck
MAX_RANGE_CM = 300           # ultrasonic readings above this mean "nothing ahead"

COLORS = {
    "NORMAL": "green",
    "PAINT_LOW": "orange",
    "PAINT_EMPTY": "red",
    "OBSTACLE_CLOSE": "red",
    "ROBOT_STUCK": "red",
    "GENERAL_ANOMALY": "orange",
}

MESSAGES = {
    "NORMAL": "All good",
    "PAINT_LOW": "Paint level low, consider refilling",
    "PAINT_EMPTY": "Paint empty, pump stopped",
    "OBSTACLE_CLOSE": "Obstacle detected, robot stopped",
    "ROBOT_STUCK": "Robot stuck, check the motors",
    "GENERAL_ANOMALY": "Anomaly detected, check the robot",
}


def generate_normal_data(n_samples=300, seed=42):
    """Synthetic normal operating conditions (no real log was available)."""
    rng = np.random.default_rng(seed)
    return np.column_stack([
        rng.uniform(20, 300, n_samples),   # obstacle distance, cm
        rng.uniform(15, 100, n_samples),   # paint level, %
        rng.uniform(60, 100, n_samples),   # speed, %
    ])


def train_model():
    """Fit the Isolation Forest and save it to models/ml_model.pkl."""
    X = generate_normal_data()
    model = IsolationForest(contamination=0.05, random_state=42, n_estimators=100)
    model.fit(X)

    MODEL_FILE.parent.mkdir(parents=True, exist_ok=True)
    with open(MODEL_FILE, "wb") as f:
        pickle.dump(model, f)
    print(f"Anomaly model trained on {len(X)} normal samples")
    return model


_model = None


def get_model():
    """Return the model, loading (or training) it once per process."""
    global _model
    if _model is None:
        if MODEL_FILE.exists():
            with open(MODEL_FILE, "rb") as f:
                _model = pickle.load(f)
        else:
            _model = train_model()
    return _model


def detect_anomaly(distance, paint, speed):
    """Classify the robot state.

    Args:
        distance: obstacle distance in cm (999 means nothing detected).
        paint: paint level in percent.
        speed: robot speed in percent.

    Returns:
        (label, confidence_percent)
    """
    # Safety rules take priority over the model.
    if paint <= PAINT_EMPTY_PCT:
        return "PAINT_EMPTY", 99.0
    if 0 < distance <= OBSTACLE_THRESHOLD_CM:
        return "OBSTACLE_CLOSE", 99.0
    if paint <= PAINT_LOW_PCT:
        return "PAINT_LOW", 95.0
    if speed <= STUCK_SPEED_PCT:
        return "ROBOT_STUCK", 90.0

    # The sensor reports 999 when no echo returns; the model was trained on
    # 20-300 cm, so clip to avoid flagging an empty field as an anomaly.
    distance = min(distance, MAX_RANGE_CM)

    model = get_model()
    X = np.array([[distance, paint, speed]])
    prediction = model.predict(X)[0]
    score = model.score_samples(X)[0]
    confidence = max(0, min(99, round((1 - (score + 0.5)) * 100, 1)))

    if prediction == -1:
        return "GENERAL_ANOMALY", confidence
    return "NORMAL", 0.0


def get_color(label):
    """Dashboard colour for a label."""
    return COLORS.get(label, "gray")


def get_message(label):
    """Human readable message for a label."""
    return MESSAGES.get(label, "Unknown state")


if __name__ == "__main__":
    train_model()
    scenarios = [
        (100, 80, 90, "Normal situation"),
        (50, 50, 85, "Normal situation 2"),
        (8, 80, 90, "Obstacle at 8 cm"),
        (12, 80, 90, "Obstacle at 12 cm"),
        (50, 2, 90, "Paint empty (2 %)"),
        (50, 8, 90, "Paint low (8 %)"),
        (50, 80, 10, "Robot stuck (speed 10 %)"),
        (999, 60, 90, "No obstacle detected"),
    ]
    for dist, paint, speed, description in scenarios:
        label, conf = detect_anomaly(dist, paint, speed)
        print(f"{description}: {label} ({conf}%) - {get_message(label)}")
