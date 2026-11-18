"""Render and replay a synthetic sliced project using the packaged CLI."""

import hashlib
from io import BytesIO
import json
import math
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

from PIL import Image


def cube(path):
    vertices = [(0, 0, 0), (10, 0, 0), (10, 10, 0), (0, 10, 0),
                (0, 0, 3), (10, 0, 3), (10, 10, 3), (0, 10, 3)]
    triangles = [(0, 2, 1), (0, 3, 2), (4, 5, 6), (4, 6, 7), (0, 1, 5),
                 (0, 5, 4), (1, 2, 6), (1, 6, 5), (2, 3, 7), (2, 7, 6),
                 (3, 0, 4), (3, 4, 7)]
    with path.open("wb") as stream:
        stream.write(b"Printable workspace capture smoke".ljust(80, b"\0"))
        stream.write(struct.pack("<I", len(triangles)))
        for triangle in triangles:
            points = [coordinate for index in triangle for coordinate in vertices[index]]
            stream.write(struct.pack("<12fH", 0, 0, 0, *points, 0))


def run(app, stage, *args):
    runtime = stage / "runtime"
    runtime.mkdir(mode=0o700)
    env = {"PATH": os.environ["PATH"], "HOME": str(stage),
           "TMPDIR": str(runtime), "XDG_RUNTIME_DIR": str(runtime),
           "LIBGL_ALWAYS_SOFTWARE": "1", "GDK_BACKEND": "x11",
           "SSL_CERT_FILE": "/etc/ssl/certs/ca-certificates.crt"}
    command = ["xvfb-run", "-a", "-s", "-screen 0 1280x1024x24 -nolisten tcp", str(app)]
    subprocess.run([*command, *args], env=env, cwd=stage, check=True, timeout=120)


def create_project(app, stage, profiles):
    stage.mkdir()
    source = stage / "source.stl"
    cube(source)
    resolved = []
    for category, name in [("machine", "Bambu Lab X1 Carbon 0.4 nozzle"),
                           ("process", "0.20mm Standard @BBL X1C"),
                           ("filament", "Bambu PLA Basic @BBL X1C")]:
        by_name = {}
        for path in (profiles / category).rglob("*.json"):
            data = json.loads(path.read_bytes())
            if data.get("name"):
                by_name[data["name"]] = data
        chain = []
        while name:
            profile = by_name[name]
            chain.append(profile)
            name = profile.get("inherits")
        settings = {}
        for profile in reversed(chain):
            settings.update(profile)
        settings.pop("inherits", None)
        settings["from"] = "system"
        destination = stage / f"{category}.json"
        destination.write_text(json.dumps(settings))
        resolved.append(str(destination))
    run(app, stage, "--slice", "1", "--arrange", "1", "--orient", "0",
        "--curr-bed-type", "Textured PEI Plate", "--export-3mf", "layout.3mf",
        "--outputdir", str(stage), "--load-settings", ";".join(resolved[:2]),
        "--load-filaments", resolved[2], str(source))
    return stage / "layout.3mf"


def capture(app, stage, project, request):
    stage.mkdir()
    request_path = stage / "view.json"
    request_path.write_text(json.dumps(request))
    run(app, stage, "--capture-workspace", str(request_path), str(project))
    metadata = json.loads((stage / "capture.json").read_bytes())
    image = (stage / "capture.png").read_bytes()
    with Image.open(BytesIO(image)) as rendered:
        assert rendered.format == "PNG"
        assert rendered.size == (request["width"], request["height"])
        assert any(low != high for low, high in rendered.convert("RGB").getextrema()), "capture is blank"
    assert metadata["kind"] == "native_orca_layout"
    assert metadata["plates"] == [1] and metadata["objects"] == [1]
    assert any(not volume["prime_tower"] for volume in metadata["volumes"]), "model is missing"
    return metadata, hashlib.sha256(image).hexdigest()


def main(app):
    profiles = Path(__file__).resolve().parents[1] / "resources/profiles/BBL"
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        project = create_project(app, root / "slice", profiles)
        original = hashlib.sha256(project.read_bytes()).hexdigest()
        request = {
            "format_version": 1, "width": 800, "height": 600, "plates": [1], "objects": [1],
            "visibility": {"beds": True, "plate_identifiers": True, "prime_towers": True},
            "camera": {"preset": "isometric", "projection": "orthographic", "margin": 1.1},
        }
        first, image = capture(app, root / "first", project, request)
        replay, replay_image = capture(app, root / "replay", project,
                                       {**request, "camera": first["camera"]})
        assert all(math.isclose(left, right, abs_tol=1e-8)
                   for first_row, replay_row in zip(first["view_matrix"], replay["view_matrix"], strict=True)
                   for left, right in zip(first_row, replay_row, strict=True))
        assert image == replay_image, "camera replay changed the rendered image"
        assert hashlib.sha256(project.read_bytes()).hexdigest() == original, "source was modified"
        print("Verified native software-rendered workspace capture and camera replay", flush=True)


if __name__ == "__main__":
    main(Path(sys.argv[1]).absolute())
