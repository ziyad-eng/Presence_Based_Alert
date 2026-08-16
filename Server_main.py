import io
import os
import re
import cv2
import wave
import time
import queue
import difflib
import numpy as np
from datetime import datetime
from faster_whisper import WhisperModel
from insightface.app import FaceAnalysis
from flask import Flask, request, jsonify, send_file



# --- Config ---
path = "" #where you put the Presence_Based_Alert folder, for example "/home/Documents/Presence_Based_Alert"
IMAGES_DIR = f"{path}/Presence_Based_Alert/data_collection/images/" 
BURSTS_DIR = f"{path}/Presence_Based_Alert/data_collectionbursts/" 
BASE_AUDIO_DIR = f"{path}/Presence_Based_Alert/data_collection/audio"
MATCH_THRESHOLD = 0.4        #per-image cosine similarity threshold for a valid match
SIMILARITY_THRESHOLD = 0.65  #for text-based name matching in transcripts
BURST_MIN_COUNT = 1          # this is the minimum number of times it must recognize someone 
                             #in a burst to be sure that they are there

print("Loading faster-whisper model (medium.en)...")
model = WhisperModel("medium.en", device="cpu", compute_type="int8", cpu_threads=4)
print("Whisper ready!")
TARGET_NAMES = ["Person1", "Person2", "Person3"]

os.makedirs(BASE_AUDIO_DIR, exist_ok=True)
for name in TARGET_NAMES + ["Unclassified"]:
    os.makedirs(os.path.join(BASE_AUDIO_DIR, name), exist_ok=True)
os.makedirs(BURSTS_DIR, exist_ok=True)

# --- Face analysis model (loads once at startup) ---
face_model = FaceAnalysis(providers=["CUDAExecutionProvider", "CPUExecutionProvider"])
face_model.prepare(ctx_id=0, det_size=(640, 640))
name_queue = queue.Queue()
latest_transcript = "No audio received yet."
latest_tags = []
whisper_status = "Idle"
total_audio_streams = 0
last_audio_duration = 0.0

SAMPLE_RATE = 16000
SAMPLE_WIDTH_BYTES = 2  # 16-bit
CHANNELS = 1


app = Flask(__name__)

def get_wav_for(name):
    """Returns (path, None) on success or (None, error_message) on failure."""
    person_dir = os.path.join(BASE_AUDIO_DIR, name)
    if not os.path.isdir(person_dir):
        return None, f"No directory for {name}"

    wavs = [f for f in os.listdir(person_dir) if f.lower().endswith(".wav")]
    if not wavs:
        return None, f"No audio files found for {name}"

    def ts(filename):
        stamp = filename.removeprefix("audio_").removesuffix(".wav")
        return datetime.strptime(stamp, "%Y%m%d_%H%M%S")

    newest = max(wavs, key=ts)
    return os.path.join(person_dir, newest), None

def classify_names_in_text(transcript_text):
    words = [word.strip(".,!?\"'") for word in transcript_text.split()]
    detected_names = set()

    for word in words:
        for target in TARGET_NAMES:
            matches = difflib.get_close_matches(word, [target], n=1, cutoff=SIMILARITY_THRESHOLD)
            if matches:
                detected_names.add(target)

    return list(detected_names)


def handle_webhook(incoming):

    if isinstance(incoming, str):
        names_to_add = [incoming]
    elif isinstance(incoming, list):
        names_to_add = incoming
    else:
        return {"error": "'confirmed_names' must be a string or a list"}

    for name in names_to_add:
        name_queue.put(name)
        print(f"Added to queue: {name}")

    return {"status": "success", "added_names": names_to_add}


def build_database(directory):
    """Build a face database from a directory of per-person subfolders.

    Expected structure:
        images/
            Halim/
                img...
            Isaac/
                img...
            Ziyad/
                img...
    """
    db = {}
    for person in os.listdir(directory):
        person_dir = os.path.join(directory, person)
        if not os.path.isdir(person_dir):
            continue
        for filename in os.listdir(person_dir):
            if not filename.lower().endswith((".jpg", ".jpeg", ".png")):
                continue
            path = os.path.join(person_dir, filename)
            img = cv2.imread(path)
            faces = face_model.get(img)
            if len(faces) == 0:
                print(f"Warning: no face in {filename}, skipping")
                continue
            best_face = max(faces, key=lambda f: f.det_score)
            db.setdefault(person, []).append(best_face.embedding)
    print(f"Database: {len(db)} people registered")
    return db


def average_embedding(db):
    """Collapse each person's list of embeddings into a single averaged, normalized embedding."""
    avg_db = {}
    for name, embeddings in db.items():
        if len(embeddings) == 0:
            print(f"Warning: no embeddings for {name}, skipping")
            continue
        avg = np.mean(embeddings, axis=0)
        avg_db[name] = avg / np.linalg.norm(avg)
    return avg_db


def identify(image_path, db, threshold=MATCH_THRESHOLD):
    """Identify all faces in a single image against the database.

    Returns a list of dicts, one per detected face, each holding the best-matching
    name (or 'Unknown'), the similarity score, per-candidate scores (debug info),
    and the bounding box.
    """
    img = cv2.imread(image_path)
    if img is None:
        return []
    faces = face_model.get(img)
    results = []

    for face in faces:
        norm_face = face.embedding / np.linalg.norm(face.embedding)
        all_scores = {}
        best_match = None
        best_score = -1

        for name, ref_emb in db.items():
            # ref_emb is already normalized by average_embedding
            score = float(np.dot(norm_face, ref_emb))
            all_scores[name] = score
            if score > best_score:
                best_score = score
                best_match = name

        label = best_match if best_score > threshold else "Unknown"
        results.append({
            "label": label,
            "score": best_score,
            "all_scores": all_scores,
            "bbox": face.bbox.astype(int).tolist(),
        })
    return results


def next_burst_dir():
    """Scan BURSTS_DIR for existing burst_NNN folders and return the next numbered path."""
    existing = [d for d in os.listdir(BURSTS_DIR) if os.path.isdir(os.path.join(BURSTS_DIR, d))]
    numbers = []
    for d in existing:
        m = re.match(r"burst_(\d+)$", d)
        if m:
            numbers.append(int(m.group(1)))
    next_num = max(numbers, default=0) + 1
    path2 = os.path.join(BURSTS_DIR, f"burst_{next_num:03d}")
    os.makedirs(path2)
    return path2


# --- Build the reference database once at startup ---
print("Building face database...")
_raw_db = build_database(IMAGES_DIR)
avg_db = average_embedding(_raw_db)

@app.route('/wav_send', methods=['GET'])
def handle_wav_send():
    if name_queue.empty():
        return "", 204

    name = name_queue.get()
    wav_path, error = get_wav_for(name)
  
    if error:
        return jsonify({"error": error}), 404

    return send_file(wav_path, mimetype="audio/wav")

@app.route("/health", methods=["GET"])
def health():
    return jsonify({"status": "ok", "people_registered": list(avg_db.keys())})


@app.route("/burst", methods=["POST"])
def burst():
    start_time = time.time()

    uploaded_files = request.files.getlist("images")
    if not uploaded_files:
        return jsonify({"error": "no images received, expected form field 'images'"}), 400

    burst_dir = next_burst_dir()

    saved_paths = []
    for i, file in enumerate(uploaded_files):
        filename = file.filename or f"img_{i}.jpg"
        path2 = os.path.join(burst_dir, filename)
        file.save(path2)
        saved_paths.append(path2)

    name_counts = {}
    per_image_debug = []

    for path2 in saved_paths:
        results = identify(path2, avg_db)
        matched_names = [r["label"] for r in results if r["label"] != "Unknown"]
        for name in matched_names:
            name_counts[name] = name_counts.get(name, 0) + 1
        per_image_debug.append({
            "image": os.path.basename(path2),
            "faces": results,
        })

    confirmed = [name for name, count in name_counts.items() if count >= BURST_MIN_COUNT]

    if confirmed: 
        handle_webhook(confirmed)  #call this to notify to handle the process
        print(f"Successfully notified server3: {confirmed}")

    elapsed = time.time() - start_time

    print(f"Burst result: {confirmed}, counts: {name_counts}")

    return jsonify({
        "burst_dir": os.path.basename(burst_dir),
        "confirmed_names": confirmed,
        "name_counts": name_counts,
        "elapsed_seconds": round(elapsed, 3),
        "debug": per_image_debug,
    })

@app.route('/transcribe', methods=['POST'])
def transcribe():
    global total_audio_streams, whisper_status, latest_transcript, latest_tags, last_audio_duration
    
    total_audio_streams += 1
    whisper_status = "Receiving Audio..."
    print("\n[Server] Receiving audio payload...")
    
    raw_pcm_bytes = request.data
    actual_length = len(raw_pcm_bytes)

    print(f"[Server] Received {actual_length} raw PCM bytes")

    if actual_length == 0:
        whisper_status = "Idle"
        print("[Server] Error: Empty audio payload.")
        return jsonify({"status": "error", "message": "Empty audio payload"}), 400

    last_audio_duration = actual_length / 32000.0
    whisper_status = "Building WAV & Transcribing with Whisper..."
    print(f"[Server] Building WAV container and running Whisper...")

    try:
        # Wrap raw PCM into memory WAV container
        wav_io = io.BytesIO()
        with wave.open(wav_io, 'wb') as wav_file:
            wav_file.setnchannels(CHANNELS)
            wav_file.setsampwidth(SAMPLE_WIDTH_BYTES)
            wav_file.setframerate(SAMPLE_RATE)
            wav_file.writeframes(raw_pcm_bytes)
        wav_io.seek(0)

        # Transcribe with Whisper
        segments, _ = model.transcribe(
            wav_io,
            beam_size=5,
            vad_filter=True
        )

        transcript_text = " ".join([seg.text.strip() for seg in segments]).strip()
        latest_transcript = transcript_text if transcript_text else "[Silence]"
        
        # Scan and tag names using fuzzy logic
        latest_tags = classify_names_in_text(transcript_text)
        
        # OS-Level Classification & Saving
        timestamp = time.strftime("%Y%m%d_%H%M%S")
        filename = f"audio_{timestamp}.wav"
        
        if latest_tags:
            for tag in latest_tags:
                folder_path = os.path.join(BASE_AUDIO_DIR, tag)
                file_path = os.path.join(folder_path, filename)
                wav_io.seek(0)
                with open(file_path, "wb") as f:
                    f.write(wav_io.read())
                print(f"[Server OS Classifier] Saved copy to folder: {tag}/")
        else:
            folder_path = os.path.join(BASE_AUDIO_DIR, "Unclassified")
            file_path = os.path.join(folder_path, filename)
            wav_io.seek(0)
            with open(file_path, "wb") as f:
                f.write(wav_io.read())
            print(f"[Server OS Classifier] Saved to folder: Unclassified/")

        whisper_status = "Idle"
        print(f"[Server] Result: {latest_transcript}")
        print(f"[Server] Classified Folders: {latest_tags if latest_tags else ['Unclassified']}\n")

        return jsonify({
            "status": "success", 
            "transcript": latest_transcript,
            "tags": latest_tags
        })

    except Exception as e:
        whisper_status = "Idle"
        print(f"[Server] Whisper Exception: {str(e)}")
        return jsonify({"status": "error", "message": str(e)}), 500


if __name__ == "__main__":
    app.run(host='0.0.0.0', port=5000, debug=False, threaded=True)
