"""Offline tests of the job.zvr handoff of KMDecimate.exe (job mode), no ZBrush needed.

usage: py -3 tools/test_job_handoff.py bin/KMDecimate.exe bench/eye3m.GoZ   (eye3m: meshgen --paint 12 --levels 2 from p3_st.obj)
"""
import os
import shutil
import struct
import subprocess
import sys
import tempfile

EXE, SRC = os.path.abspath(sys.argv[1]), os.path.abspath(sys.argv[2])
fails = 0


def check(name, ok, info=""):
    global fails
    print(("PASS " if ok else "FAIL ") + name + ("  " + info if info and not ok else ""))
    fails += not ok


def zvr(values):
    """What ZScript VarSave writes for a numeric array: e9 03 00 00, count, ('F', float32) per element."""
    b = struct.pack("<II", 1001, len(values))
    for v in values:
        b += b"F" + struct.pack("<f", v)
    return b


def has_mrgb(path):
    d = open(path, "rb").read()
    pos = 32
    while pos + 8 <= len(d):
        tag, size = struct.unpack_from("<II", d, pos)
        if tag == 0 or size < 8:
            break
        if tag == 35001:
            return True
        pos += size
    return False


work = tempfile.mkdtemp(prefix="kd_zvr_") + "\\"
log = os.path.join(work, "log.txt")


def run(job_file, env_extra=None):
    for f in ("status.txt", "note.txt", "out.GoZ"):
        if os.path.exists(work + f):
            os.remove(work + f)
    shutil.copy(SRC, work + "in.GoZ")
    env = dict(os.environ)
    env.update(env_extra or {})
    subprocess.run([EXE, job_file], env=env, timeout=300)
    status = open(work + "status.txt").read() if os.path.exists(work + "status.txt") else ""
    note = open(work + "note.txt", encoding="utf-8").read() if os.path.exists(work + "note.txt") else ""
    last = open(log, encoding="utf-8").read().strip().splitlines()[-1] if os.path.exists(log) else ""
    return status, note, last


# 1. color on (detail 8), run id and a MemSaveToFile result in the job
open(work + "job.zvr", "wb").write(zvr([2, 5, 1, 0, 8, -5, 1234.5, 0]))
st, note, last = run(work + "job.zvr")
check("zvr color on: finished", st == "1", st + note)
check("zvr color on: color=8 and job=zvr in the log", "color=8 " in last and "job=zvr jobtxt=-5" in last, last)
check("zvr color on: output has polypaint", os.path.exists(work + "out.GoZ") and has_mrgb(work + "out.GoZ"))
check("zvr color on: note says colors kept", "保留了顶点色" in note, note)

# 2. color off
open(work + "job.zvr", "wb").write(zvr([2, 5, 0, 0, 0, 0, 1235, 0]))
st, note, last = run(work + "job.zvr")
check("zvr color off: color=0, no polypaint", st == "1" and "color=0 " in last and not has_mrgb(work + "out.GoZ"), last)
check("zvr color off: groups=0 from the job", "groups=0" in last, last)

# 3. Esc (simulated) -> status 8, no output
open(work + "job.zvr", "wb").write(zvr([2, 5, 1, 0, 8, 0, 1236, 0]))
st, note, last = run(work + "job.zvr", {"KMDEC_TEST_CANCEL": "0"})
check("zvr cancel: status 8, no output", st == "8" and not os.path.exists(work + "out.GoZ"), st + " " + note)

# 4. a broken job.zvr falls back to this run's job.txt
open(work + "job.zvr", "wb").write(b"garbage")
open(work + "job.txt", "w", newline="").write(
    "KMDEC1\r\n%sin.GoZ\r\n%sout.GoZ\r\n10\r\n1\r\n0\r\n3\r\nEND\r\n" % (work, work))
st, note, last = run(work + "job.zvr")
check("broken zvr -> job.txt", st == "1" and "percent=10 " in last and "color=3 " in last and "job=txt(zvr:" in last, last)

# 5. no job at all -> clear error, never an old job
os.remove(work + "job.zvr")
os.remove(work + "job.txt")
st, note, last = run(work + "job.zvr")
check("no job: status 9 with a message", st == "9" and "job.zvr" in note, st + " " + note)

# 6. the old plugin still launches with job.txt
open(work + "job.txt", "w", newline="").write(
    "KMDEC1\r\n%sin.GoZ\r\n%sout.GoZ\r\n5\r\n0\r\n0\r\nEND\r\n" % (work, work))
st, note, last = run(work + "job.txt")
check("old job.txt (no color line) still works, keeps colors", st == "1" and "color=1 " in last and "job=txt" in last, last)

# 7. timings of the previous run: zb_last.zvr -> one line in zb_times.txt, then deleted
open(work + "zb_last.zvr", "wb").write(zvr([3, 0.47, 6.3, 2.0, 1, 11.73, 0, 10, 5, -1, 0.3, 1.7, 98765, 0]))
open(work + "job.zvr", "wb").write(zvr([2, 5, 0, 0, 10, 0, 98766, 0]))
st, note, last = run(work + "job.zvr")
times = open(work + "zb_times.txt", encoding="utf-8").read() if os.path.exists(work + "zb_times.txt") else ""
check("zb_last.zvr appended to zb_times.txt",
      "export=0.47 compute=6.30 import=2.00 import1=0.30 import2=1.70 reimport=1 total=11.73 copy=0 color=10 percent=5 jobtxt=-1 run=98765" in times,
      times)
check("zb_last.zvr deleted afterwards", not os.path.exists(work + "zb_last.zvr"))
st, note, last = run(work + "job.zvr")
times2 = open(work + "zb_times.txt", encoding="utf-8").read()
check("not appended twice", times2.count("run=98765") == 1, times2)

# 8. remesh pre-decimation: element 7 = target triangles (the percent is ignored), groups kept, no polypaint
open(work + "job.zvr", "wb").write(zvr([2, 0.1, 1, 0, 0, 0, 1237, 50000]))
st, note, last = run(work + "job.zvr")
out_tris = int(last.split("out_tris=")[1].split()[0]) if "out_tris=" in last else 0
check("target triangles: about 50000 out, percent ignored", st == "1" and abs(out_tris - 50000) < 500 and "target=50000" in last, last)
check("target triangles: groups kept, no polypaint", "groups=1" in last and not has_mrgb(work + "out.GoZ"), last)

print("%d failure(s)" % fails)
shutil.rmtree(work, ignore_errors=True)
sys.exit(1 if fails else 0)
