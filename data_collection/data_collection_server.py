from flask import Flask, request, send_from_directory, abort
from datetime import datetime
import os

app = Flask(__name__)
IMAGE_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "images")
os.makedirs(IMAGE_DIR, exist_ok=True)


@app.route("/upload", methods=["POST"])
def upload():
    # ESP32 posting raw JPEG bytes with Content-Type: image/jpeg
    data = request.get_data()
    if not data:
        return "no data received", 400

    filename = datetime.now().strftime("%Y%m%d_%H%M%S_%f") + ".jpg"
    filepath = os.path.join(IMAGE_DIR, filename)
    with open(filepath, "wb") as f:
        f.write(data)

    print(f"Saved {filename} ({len(data)} bytes)")
    return "ok", 200


@app.route("/")
def gallery():
    files = sorted(os.listdir(IMAGE_DIR), reverse=True)
    files = [f for f in files if f.lower().endswith(".jpg")]

    thumbs = "".join(
        f'''
        <div style="display:inline-block; margin:8px; text-align:center;">
            <a href="/images/{f}" download>
                <img src="/images/{f}" style="width:200px; height:150px; object-fit:cover; border:1px solid #ccc;">
            </a>
            <div style="font-size:12px;">{f}</div>
        </div>
        '''
        for f in files
    )

    html = f"""
    <html>
    <head><title>Enrollment images ({len(files)})</title></head>
    <body style="font-family: sans-serif;">
        <h2>Captured images: {len(files)}</h2>
        {thumbs if files else "<p>No images yet.</p>"}
    </body>
    </html>
    """
    return html


@app.route("/images/<path:filename>")
def serve_image(filename):
    if "/" in filename or ".." in filename:
        abort(400)
    return send_from_directory(IMAGE_DIR, filename)


if __name__ == "__main__":
    # 0.0.0.0 so it's reachable from the ESP32 over your LAN, not just localhost
    app.run(host="0.0.0.0", port=5000, debug=True)