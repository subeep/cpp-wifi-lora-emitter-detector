"""Read-only sources, isolated outputs and deterministic provenance."""
import hashlib
import json
import os
from pathlib import Path
import stat


def stamp(path):
    s = Path(path).stat()
    return [s.st_dev, s.st_ino, s.st_size, s.st_atime_ns, s.st_mtime_ns, s.st_ctime_ns]


def read_source(path):
    fd = os.open(path, os.O_RDONLY | os.O_NOATIME | os.O_NONBLOCK)
    if not stat.S_ISREG(os.fstat(fd).st_mode):
        os.close(fd)
        raise ValueError('source must be a regular file')
    return os.fdopen(fd, 'rb')


def sha256(path):
    h = hashlib.sha256()
    with read_source(path) as f:
        for block in iter(lambda: f.read(1024 * 1024), b''):
            h.update(block)
    return h.hexdigest()


def new_output(path, source_roots):
    path = Path(path).resolve()
    for root in source_roots:
        root = Path(root).resolve()
        if path == root or root in path.parents:
            raise ValueError('output must be outside all source roots')
    path.mkdir(parents=True, exist_ok=False)
    return path


def write_json(path, obj):
    Path(path).write_text(json.dumps(obj, indent=2, allow_nan=False) + '\n')


def identity(path, member=None):
    return hashlib.sha256((str(Path(path).resolve()) + '\0' + (member or '')).encode()).hexdigest()[:24]
