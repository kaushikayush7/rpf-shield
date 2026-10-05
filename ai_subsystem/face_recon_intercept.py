"""
FIRMWARE 4 (v2) - face_recon_intercept.py : advisory recognition + operator-armed tracking
pip install opencv-python deepface scipy requests numpy tf-keras

Keys: a = ARM / DISARM autonomous steering (default DISARMED)
      q = quit
"""
import os, cv2, time, queue, threading, requests, numpy as np, urllib.request
from deepface import DeepFace
from scipy.spatial.distance import cosine

CAMERA_STREAM_URL = "http://192.168.1.60:81/" # ESP32-CAM
MAIN_ESP32_URL    = "http://192.168.1.50"    # Main ESP32
MATCH_THRESHOLD   = 0.40 # Facenet cosine distance
TENSION_LIMIT     = 35
NO_FACE_TIMEOUT   = 1.0  # s without a face -> command 'S'

XML_FILE = "haarcascade_frontalface_default.xml"
if not os.path.exists(XML_FILE):
    urllib.request.urlretrieve("https://raw.githubusercontent.com/opencv/opencv/master/data/"
                               "haarcascades/haarcascades/haarcascade_frontalface_default.xml", XML_FILE)

face_cascade = cv2.CascadeClassifier(XML_FILE)
clahe = cv2.createCLAHE(clipLimit=2.0, tileGridSize=(8, 8))

frame_queue = queue.Queue(maxsize=2)
task_queue  = queue.Queue(maxsize=1)
lock = threading.Lock()

latest = None # latest detection dict
last_face_time = 0.0
armed = False
session_faces = [] # embeddings live in RAM only for this session (privacy)
person_counter = 1

print("[AI] Loading models...")
dummy = np.zeros((160, 160, 3), dtype=np.uint8)
DeepFace.represent(dummy, model_name="Facenet", enforce_detection=False)
DeepFace.analyze(dummy, actions=["emotion"], enforce_detection=False)

def stream_capture_thread():
    while True:
        try:
            stream = urllib.request.urlopen(CAMERA_STREAM_URL, timeout=5)
            buf = b""
            while True:
                chunk = stream.read(4096)
                if not chunk: break
                buf += chunk
                a = buf.find(b"\xff\xd8")
                b = buf.find(b"\xff\xd9", a if a != -1 else 0)
                if a != -1 and b != -1:
                    jpg = buf[a:b+2]
                    buf = buf[b+2:]
                    frame = cv2.imdecode(np.frombuffer(jpg, np.uint8), cv2.IMREAD_COLOR)
                    if frame is not None:
                        if frame_queue.full():
                            try: frame_queue.get_nowait()
                            except queue.Empty: pass
                        frame_queue.put(frame)
                elif a == -1 and len(buf) > 200000:
                    buf = b""
        except Exception:
            time.sleep(2)

def send_nav(cmd, state, ident):
    try:
        requests.get(f"{MAIN_ESP32_URL}/ai_nav",
                     params={"cmd": cmd, "state": state, "id": ident, "arm": int(armed)}, timeout=0.5)
    except Exception:
        pass

def ai_worker_thread():
    global person_counter, latest, last_face_time
    while True:
        crop, box, W, H = task_queue.get()
        rx, ry, rw, rh = box
        ident, acc = "Subject", 100.0
        try:
            emb = DeepFace.represent(crop, model_name="Facenet", enforce_detection=False)[0]["embedding"]
            best_id, best_d = None, 1.0
            for e in session_faces:
                d = cosine(emb, e["embedding"])
                if d < best_d: best_d, best_id = d, e["id"]
            if best_id is not None and best_d < MATCH_THRESHOLD:
                ident, acc = best_id, round((1 - best_d) * 100, 1)
            else:
                ident = f"Subject #{person_counter}"; person_counter += 1
                session_faces.append({"id": ident, "embedding": emb})
        except Exception:
            pass

        emotion, tension = "neutral", 0
        try:
            em = DeepFace.analyze(crop, actions=["emotion"], enforce_detection=False)[0]
            emotion = em["dominant_emotion"]; e = em["emotion"]
            tension = int(min(100, max(0, e.get("fear", 0) + e.get("angry", 0) + 0.4 * e.get("sad", 0))))
        except Exception:
            pass

        tense = tension >= TENSION_LIMIT
        cx, mid = rx + rw // 2, W // 2
        cmd = "S"
        if tense:
            if cx < mid - 35:
                cmd = "L"
            elif cx > mid + 35:
                cmd = "R"
            else:
                cmd = "F" if rw < 75 else "C"

        send_nav(cmd, "STRESSED" if tense else "RELAXED", ident)
        with lock:
            latest = dict(box=box, id=ident, acc=acc, emotion=emotion, tension=tension, tense=tense, cmd=cmd)
            last_face_time = time.time()
        task_queue.task_done()

threading.Thread(target=stream_capture_thread, daemon=True).start()
threading.Thread(target=ai_worker_thread, daemon=True).start()

print("[READY] Disarmed (advisory) mode. Press 'a' to arm steering, 'q' to quit.")
n = 0
while True:
    try:
        frame = frame_queue.get(timeout=2.0)
    except queue.Empty:
        continue

    n += 1
    disp = frame.copy()
    h, w, _ = frame.shape

    if n % 3 == 0:
        g = cv2.cvtColor(frame, cv2.COLOR_BGR2GRAY)
        faces = face_cascade.detectMultiScale(g, 1.1, 4, minSize=(45, 45))
        if len(faces) > 0 and task_queue.empty():
            rx, ry, rw, rh = max(faces, key=lambda f: f[2] * f[3]) # largest face
            p = int(rw * 0.1)
            crop = frame[max(0, ry-p):min(h, ry+rh+p), max(0, rx-p):min(w, rx+rw+p)].copy()
            if crop.size: task_queue.put((crop, (rx, ry, rw, rh), w, h))

    # Failsafe: person left the frame -> tell the robot to stand
    with lock:
        det = latest
        stale = (time.time() - last_face_time) > NO_FACE_TIMEOUT

    if stale and det is not None and n % 10 == 0:
        send_nav("S", "RELAXED", "none")
        with lock:
            latest = None
            det = None

    if det:
        rx, ry, rw, rh = det["box"]
        col = (0, 0, 255) if det["tense"] else (0, 255, 120)
        cv2.rectangle(disp, (rx, ry), (rx + rw, ry + rh), col, 2)
        cv2.putText(disp, f'{det["id"]} [{det["acc"]}%]', (rx, max(25, ry - 30)), 0, 0.6, (255, 255, 255), 2)
        cv2.putText(disp, f'TENSION {det["tension"]}% [{det["emotion"].upper()}] CMD: {det["cmd"]}',
                    (rx, max(45, ry - 8)), 0, 0.45, col, 2)

    mode = "ARMED" if armed else "ADVISORY (robot will not steer)"
    cv2.putText(disp, f"RPF-SHIELD HUD | {mode}", (15, 25), 0, 0.55, (0, 0, 255) if armed else (220, 220, 220), 2)
    cv2.imshow("RPF Quadruped Recon HUD", disp)

    k = cv2.waitKey(1) & 0xFF
    if k == ord("q"):
        break
    if k == ord("a"):
        armed = not armed

cv2.destroyAllWindows()
