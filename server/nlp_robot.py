"""Intent classifier for natural-language robot commands.

TF-IDF on character n-grams + Multinomial Naive Bayes. It runs locally with no
internet access and tolerates typos. The training phrases are French.

Intents: football, stop, pause, resume, paint_level, status, pump_test, scan
"""

import pickle
import re
import unicodedata
from pathlib import Path

from sklearn.feature_extraction.text import TfidfVectorizer
from sklearn.naive_bayes import MultinomialNB
from sklearn.pipeline import Pipeline

MODEL_FILE = Path(__file__).resolve().parent.parent / "models" / "nlp_model.pkl"

TRAINING_DATA = {
    "football": [
        "trace un terrain de foot", "trace un terrain de football",
        "je veux un terrain de football", "dessine un terrain de foot",
        "tracer football", "terrain de football", "terrain de foot",
        "je veux du football", "faire un terrain foot",
        "terrain football sil te plait", "dessine le terrain de foot",
        "commencer le tracage football", "lancer le terrain football",
        "tracer le terrain", "debut tracage football", "start football",
        "trace le foot", "footbal", "tererrain de foot", "je veux tracer foot",
    ],
    "stop": [
        "stop", "arrete", "arrete tout", "stoppe", "stop robot", "arret",
        "stoppe le robot", "arrete le robot", "stopper", "stop stop", "halte",
        "arrete immediatement", "stop urgent", "urgence stop", "couper tout",
        "tout arreter", "stope", "aret", "arrete tou", "stop sil te plait",
    ],
    "pause": [
        "pause", "mets en pause", "mettre en pause", "suspendre", "attends",
        "pauser", "faire une pause", "suspendre le robot", "attendre",
        "pause sil te plait", "met en pause", "suspends", "pauze",
        "mets pause", "faire pause",
    ],
    "resume": [
        "reprendre", "continue", "continuer", "relancer", "reprends",
        "reprendre le tracage", "continuer le tracage", "relancer le robot",
        "go", "reprend", "continut", "reprendr", "continuer sil te plait",
        "on continue", "repart",
    ],
    "paint_level": [
        "niveau peinture", "combien de peinture", "peinture restante",
        "verifie peinture", "verifier la peinture",
        "il reste combien de peinture", "peinture niveau", "check peinture",
        "quantite peinture", "jauge peinture", "peinture vide",
        "reservoir peinture", "niveau reservoir", "combien reste peinture",
        "etat peinture",
    ],
    "status": [
        "statut", "etat du robot", "comment va le robot", "infos robot",
        "status", "donne moi le statut", "quel est letat", "info robot",
        "etat robot", "robot status", "comment est le robot",
        "verifier robot", "check robot", "diagnostic robot", "rapport robot",
    ],
    "pump_test": [
        "test pompe", "tester la pompe", "verifie la pompe", "essai pompe",
        "tester pompe", "pompe test", "verifier pompe", "check pompe",
        "faire test pompe", "pompe marche", "essayer pompe",
        "test de la pompe", "teste la pompe", "tets pompe", "pompe verifier",
    ],
    "scan": [
        "scanner", "scan", "verifie obstacle", "regarder autour",
        "scanner environnement", "detecter obstacle", "chercher obstacle",
        "scan autour", "faire un scan", "scanner les obstacles",
        "verifier obstacles", "check obstacle", "scan sil te plait", "scane",
        "regarder les obstacles",
    ],
}


def normalize(text):
    """Lowercase, strip accents and punctuation."""
    text = unicodedata.normalize("NFKD", text.lower().strip())
    text = "".join(c for c in text if not unicodedata.combining(c))
    text = re.sub(r"[^a-z0-9 ]", " ", text)
    return re.sub(r"\s+", " ", text).strip()


def train_model():
    """Fit the pipeline and save it to models/nlp_model.pkl."""
    phrases = [normalize(p) for intent in TRAINING_DATA.values() for p in intent]
    labels = [intent for intent, items in TRAINING_DATA.items() for _ in items]

    pipeline = Pipeline([
        ("tfidf", TfidfVectorizer(analyzer="char_wb", ngram_range=(2, 4),
                                  min_df=1, max_features=5000)),
        ("clf", MultinomialNB(alpha=0.1)),
    ])
    pipeline.fit(phrases, labels)

    MODEL_FILE.parent.mkdir(parents=True, exist_ok=True)
    with open(MODEL_FILE, "wb") as f:
        pickle.dump(pipeline, f)
    print(f"NLP model trained: {len(phrases)} phrases, {len(TRAINING_DATA)} intents")
    return pipeline


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


def predict_intent(text):
    """Return (intent, confidence_percent) for a command."""
    model = get_model()
    clean = normalize(text)
    intent = model.predict([clean])[0]
    confidence = round(float(model.predict_proba([clean]).max()) * 100, 1)
    return intent, confidence


if __name__ == "__main__":
    train_model()
    tests = [
        "trace un terrain de football", "stop", "mets en pause", "reprendre",
        "niveau peinture", "statut du robot", "test pompe", "scanner",
        "tererrain de foot", "aret tout", "pauze", "stope", "tets pompe", "scane",
    ]
    for text in tests:
        intent, conf = predict_intent(text)
        print(f"{text!r} -> {intent} ({conf}%)")
