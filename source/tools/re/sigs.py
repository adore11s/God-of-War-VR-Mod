# D-187: byte signatures for every GoW.exe address the mod uses, so it can find them again in another build
# (another store's exe, an update). Writes src/core/addrsigs.inc.
#
#   python tools/re/sigs.py [path\to\GoW.exe]
#
# The addresses are taken from the mod's own source (0x14xxxxxxx constants outside comments, inside the image).
#  - code (.text): the bytes AT the address, the moving parts wildcarded (rel32 branch / call targets, RIP-relative
#    displacements), whole instructions added until the pattern occurs exactly once in .text;
#  - data (.rdata / .data / .bss): a .text instruction that addresses it RIP-relative (the address = that instruction's
#    end + its displacement [+ a delta when only a nearby address is referenced]) with enough instructions after it to
#    be unique, displacements wildcarded the same way.
# Each entry: {staticVa, kind (0 code / 1 data / 2 section), dispOff, insnEnd, delta, "pattern"} - for code the address is the
# match + dispOff (0, or the length of a neighbour prefix); for data it is match + insnEnd + int32(match + dispOff) + delta.
import os, re, sys, hashlib
import numpy as np
import pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_64
from capstone.x86 import X86_OP_MEM, X86_OP_IMM, X86_REG_RIP

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
EXE = sys.argv[1] if len(sys.argv) > 1 else os.path.normpath(os.path.join(ROOT, "..", "GoW.exe"))
OUT = os.path.join(ROOT, "src", "core", "addrsigs.inc")
MAXLEN = 96

pe = pefile.PE(EXE, fast_load=True)
IB = pe.OPTIONAL_HEADER.ImageBase
SIZE = pe.OPTIONAL_HEADER.SizeOfImage
text = next(s for s in pe.sections if s.Name.rstrip(b"\0") == b".text")
TVA = IB + text.VirtualAddress
T = text.get_data()[: text.Misc_VirtualSize]
md = Cs(CS_ARCH_X86, CS_MODE_64)
md.detail = True

# ---- addresses used by the mod's code (comments stripped) -------------------------------------------------------------
def strip_comments(s):
    s = re.sub(r"/\*.*?\*/", " ", s, flags=re.S)
    out = []
    for line in s.splitlines():
        # drop // comments outside string literals (good enough for this code base)
        q = False
        for i, ch in enumerate(line):
            if ch == '"' and (i == 0 or line[i - 1] != "\\"):
                q = not q
            if not q and line.startswith("//", i):
                line = line[:i]
                break
        out.append(line)
    return "\n".join(out)

used = {}
for dp, _, fs in os.walk(os.path.join(ROOT, "src")):
    for f in fs:
        if not f.endswith((".cpp", ".h")) or f == "addrsigs.inc":
            continue
        src = strip_comments(open(os.path.join(dp, f), encoding="utf-8", errors="replace").read())
        for m in re.finditer(r"0x(14[0-9a-fA-F]{7})(?:ull|ULL|u)?\b", src):
            va = int(m.group(1), 16)
            if IB < va < IB + SIZE:
                used.setdefault(va, set()).add(os.path.relpath(os.path.join(dp, f), ROOT))

def in_text(va):
    return TVA <= va < TVA + len(T)

# ---- pattern helpers ------------------------------------------------------------------------------------------------
def insn_mask(ins):
    """bytes of one instruction, None where the build-dependent fields are (rel / RIP displacements)"""
    b = list(ins.bytes)
    m = [True] * len(b)
    if ins.disp_offset and any(op.type == X86_OP_MEM and op.mem.base == X86_REG_RIP for op in ins.operands):
        for k in range(ins.disp_offset, ins.disp_offset + ins.disp_size):
            m[k] = False
    if ins.group(7) or ins.group(2) or ins.group(1):  # relative branch / call / jump
        if ins.imm_offset and ins.imm_size:
            for k in range(ins.imm_offset, ins.imm_offset + ins.imm_size):
                m[k] = False
    return [x if keep else None for x, keep in zip(b, m)]

def regex_of(pat):
    return re.compile(b"".join(b"." if x is None else re.escape(bytes([x])) for x in pat), re.S)

def count(pat, limit=2):
    n = 0
    for _ in regex_of(pat).finditer(T):
        n += 1
        if n >= limit:
            break
    return n

def build(start_va, first_needed):
    """pattern from start_va: whole instructions, at least first_needed bytes, until unique"""
    off = start_va - TVA
    pat = []
    for ins in md.disasm(T[off: off + MAXLEN + 16], start_va):
        pat += insn_mask(ins)
        if len(pat) >= max(first_needed, 12) and count(pat) == 1:
            return pat
        if len(pat) >= MAXLEN:
            break
    return None

def fmt(pat):
    return " ".join("??" if x is None else "%02x" % x for x in pat)

# ---- data references: every 4-byte window of .text read as a RIP displacement ---------------------------------------
u8 = np.frombuffer(T, dtype=np.uint8)
n4 = len(u8) - 3
disp = (u8[:n4].astype(np.int64) | (u8[1:n4 + 1].astype(np.int64) << 8) | (u8[2:n4 + 2].astype(np.int64) << 16) |
        (u8[3:n4 + 3].astype(np.int64) << 24))
disp = np.where(disp >= 1 << 31, disp - (1 << 32), disp)
endva = TVA + np.arange(n4, dtype=np.int64) + 4 + disp  # target if the instruction ends right after the displacement

def references(target):
    """(insn start va, insn) for instructions that address target RIP-relative"""
    hits = []
    for k in (0, 1, 2, 4):  # an immediate of k bytes after the displacement
        for p in np.nonzero(endva + k == target)[0][:400]:
            p = int(p)
            for s in range(p - 1, max(p - 9, -1), -1):
                insn = next(md.disasm(T[s: s + 16], TVA + s), None)
                if not insn or not insn.disp_offset or insn.disp_offset != p - s or                         insn.size - insn.disp_offset - insn.disp_size != k:
                    continue
                if not any(op.type == X86_OP_MEM and op.mem.base == X86_REG_RIP for op in insn.operands):
                    continue
                if insn.address + insn.size + insn.disp == target:
                    hits.append((insn, insn.disp_offset))
                    break
            # a relative call / jump to it (rel32 = the instruction's last 4 bytes)
            if k == 0 and p >= 1 and T[p - 1] in (0xE8, 0xE9):
                insn = next(md.disasm(T[p - 1: p + 4], TVA + p - 1), None)
                if insn and insn.size == 5 and insn.operands and insn.operands[0].type == X86_OP_IMM and                         insn.operands[0].imm == target:
                    hits.append((insn, 1))
    return hits


# .pdata: the function laid out right before an address (its last instructions + the padding up to it)
_funcs = None
def prefix_before(va, keep=48):
    global _funcs
    if _funcs is None:
        pd = next(x for x in pe.sections if x.Name.rstrip(b"\0") == b".pdata").get_data()
        a = np.frombuffer(pd[: len(pd) // 12 * 12], dtype=np.uint32).reshape(-1, 3)
        _funcs = sorted(set(int(b) + IB for b in a[:, 0] if b))
    import bisect
    i = bisect.bisect_left(_funcs, va) - 1
    if i < 0:
        return None
    start = _funcs[i]
    if not in_text(start) or va - start > 0x4000:
        return None
    masked, pos = [], start
    for ins in md.disasm(T[start - TVA: va - TVA], start):
        masked.append((ins.address, insn_mask(ins)))
        pos = ins.address + ins.size
    tail = [x for _, m in masked for x in m]
    tail += list(T[pos - TVA: va - TVA])  # padding (int3) after the last decoded instruction
    return tail[-keep:] if len(tail) > keep else tail

entries, problems = [], []
for va in sorted(used):
    sec = next((x for x in pe.sections if IB + x.VirtualAddress == va), None)
    if sec is not None:  # a section's start: resolved from the section table by name
        entries.append((va, 2, 0, 0, 0, list(sec.Name.rstrip(b"\0"))))
        continue
    if in_text(va):
        pat = build(va, 16)
        if pat:
            entries.append((va, 0, 0, 0, 0, pat))
            continue
        # too generic on its own (a short function): a unique call / jump to it instead
        best = None
        for ins, do in references(va)[:60]:
            p2 = build(ins.address, ins.size)
            if p2 and (best is None or len(p2) < len(best[2])):
                best = (ins, do, p2)
        if best:
            entries.append((va, 1, best[1], best[0].size, 0, best[2]))
            continue
        # only called through pointers: anchored on the end of the function laid out before it (.pdata) + its own bytes
        pre = prefix_before(va)
        if pre:
            off = len(pre)
            q = pre[:]
            for ins in md.disasm(T[va - TVA: va - TVA + MAXLEN + 16], va):
                q += insn_mask(ins)
                if count(q) == 1:
                    entries.append((va, 0, off, 0, 0, q))
                    break
                if len(q) - off >= MAXLEN:
                    q = None
                    break
            else:
                q = None
            if q:
                continue
        problems.append((va, "code: no unique pattern, no unique call to it, no unique neighbour"))
        continue
    done = False
    for delta in list(range(0, 0x41)) + list(range(0x48, 0x1001, 8)):
        refs = references(va - delta)
        best = None
        for ins, do in refs[:60]:
            pat = build(ins.address, ins.size)
            if pat and (best is None or len(pat) < len(best[2])):
                best = (ins, do, pat)
        if best:
            ins, do, pat = best
            entries.append((va, 1, do, ins.size, delta, pat))
            done = True
            break
    if not done:
        problems.append((va, "data: no referencing instruction found"))

sha = hashlib.sha256(open(EXE, "rb").read()).hexdigest()
with open(OUT, "w", encoding="utf-8", newline="\n") as f:
    f.write("// GENERATED by tools/re/sigs.py from GoW.exe sha256 %s - do not edit by hand.\n" % sha)
    f.write("// {staticVa, kind, dispOff, insnEnd, delta, pattern}  (D-187)\n")
    f.write("//  kind 0 code: the address = match + dispOff (0, or the length of a neighbour-function prefix)\n")
    f.write("//  kind 1 referenced: match + insnEnd + int32 at (match + dispOff) + delta (a RIP-relative use or a rel32 call)\n")
    f.write("//  kind 2 section start: pattern = the section's name\n")
    for va, kind, do, ie, de, pat in entries:
        body = bytes(pat).decode() if kind == 2 else fmt(pat)  # a section: its name
        f.write('{0x%xull, %d, %d, %d, %d, "%s"},  // %s\n' % (va, kind, do, ie, de, body, ", ".join(sorted(used[va]))))
print("addresses used:", len(used), "| signatures:", len(entries), "| problems:", len(problems))
for va, why in problems:
    print("  0x%x %s (%s)" % (va, why, ", ".join(sorted(used[va]))))
print("wrote", OUT)
