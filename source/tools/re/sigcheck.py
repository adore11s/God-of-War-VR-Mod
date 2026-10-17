# D-187: check src/core/addrsigs.inc against a GoW.exe the way the mod's resolver (core/addr.cpp) uses it: every
# signature must lead to exactly one address. On the recorded build that address must be the recorded one.
#   python tools/re/sigcheck.py [path\to\GoW.exe]
import os, re, sys, struct
import pefile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
EXE = sys.argv[1] if len(sys.argv) > 1 else os.path.normpath(os.path.join(ROOT, "..", "GoW.exe"))
pe = pefile.PE(EXE, fast_load=True)
IB = pe.OPTIONAL_HEADER.ImageBase
text = next(s for s in pe.sections if s.Name.rstrip(b"\0") == b".text")
TVA = IB + text.VirtualAddress
T = text.get_data()[: text.Misc_VirtualSize]
rows = re.findall(r'\{0x([0-9a-f]+)ull, (\d), (\d+), (\d+), (-?\d+), "([^"]*)"\}',
                  open(os.path.join(ROOT, "src", "core", "addrsigs.inc"), encoding="utf-8").read())
same = moved = missing = ambiguous = 0
for va, kind, do, ie, de, pat in rows:
    va, kind, do, ie, de = int(va, 16), int(kind), int(do), int(ie), int(de)
    if kind == 2:
        sec = next((s for s in pe.sections if s.Name.rstrip(b"\0") == pat.encode()), None)
        got = {IB + sec.VirtualAddress} if sec else set()
    else:
        rx = re.compile(b"".join(b"." if t == "??" else re.escape(bytes([int(t, 16)])) for t in pat.split()), re.S)
        got = set()
        n = 0
        for m in rx.finditer(T):
            n += 1
            at = TVA + m.start()
            if kind == 0:
                got.add(at + do)
            else:
                d = struct.unpack_from("<i", T, m.start() + do)[0]
                got.add(at + ie + d + de)
    if len(got) == 1:
        g = got.pop()
        if g == va:
            same += 1
        else:
            moved += 1
            print("MOVED 0x%x -> 0x%x" % (va, g))
    elif not got:
        missing += 1
        print("missing 0x%x" % va)
    else:
        ambiguous += 1
        print("ambiguous 0x%x (%d places)" % (va, len(got)))
print("%d signatures: %d at the recorded place, %d moved, %d missing, %d ambiguous" % (len(rows), same, moved, missing, ambiguous))
