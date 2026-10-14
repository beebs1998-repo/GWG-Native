"""Inventory the indirect calls game.exe makes through vtable pointers.

The rotation work is blocked on one question: which IDirect3DDevice8 vtable
slot is Present. The header available here disagrees with the runtime, so the
answer has to come from the game itself - the binary encodes every vtable
dispatch as a literal displacement, so the call sites *are* the answer.

Scans for the exact opcode pattern of `call dword ptr [reg+disp]` (FF /2 with a
memory operand) rather than disassembling the section linearly, which desyncs on
a 1.5 MB binary and silently loses most of the results.

For each site it records the displacement (the candidate vtable index) and how
many arguments were pushed immediately before the call. Present takes four,
which should separate it from the rest.

Usage: python callsites.py <game.exe> [out.txt]
"""
import sys
from collections import defaultdict

import pefile
import capstone

MAX_DISP = 0x400  # beyond this it is almost certainly a struct field, not a slot


def scan_call_sites(text, text_va):
    """Yield (offset, disp, reg_name) for every `call [reg+disp]` in the blob.

    FF /2 where /2 is the call group of the two-byte opcode. The ModRM byte's
    reg field selects the operation and its mod field the addressing mode, so a
    memory operand with mod != 3 and reg == 2 is exactly the pattern wanted.
    """
    reg_of_rm = {0: "eax", 1: "ecx", 2: "edx", 3: "ebx", 4: "esp", 5: "ebp",
                 6: "esi", 7: "edi"}
    i = 0
    n = len(text)
    while i < n - 1:
        if text[i] != 0xFF:
            i += 1
            continue
        modrm = text[i + 1]
        mod = modrm >> 6
        reg = (modrm >> 3) & 7
        rm = modrm & 7
        if reg != 2 or mod == 3:
            i += 1
            continue
        # mod==0 with rm==5 is an absolute disp32, not a register+displacement
        if mod == 0 and rm == 5:
            i += 1
            continue
        if mod == 1:
            if i + 3 > n:
                break
            disp = text[i + 2]
            if disp & 0x80:
                disp -= 0x100
            size = 3
        elif mod == 2:
            if i + 6 > n:
                break
            disp = int.from_bytes(text[i + 2:i + 6], "little", signed=True)
            size = 6
        else:  # mod == 0, [reg]
            disp = 0
            size = 2

        if 0 < disp <= MAX_DISP and disp % 4 == 0:
            yield i, disp, reg_of_rm.get(rm, "?")
        i += size


def count_pushes_before(md, text, base, call_offset, back_bytes=64):
    """How many pushes sit contiguously before this call.

    Walks backwards over pushes, tolerating a few instructions that commonly
    sit between the last push and the call. Used to separate 4-argument calls
    from 1- and 2-argument ones, not as an exact count.
    """
    start = max(0, call_offset - back_bytes)
    chunk = list(md.disasm(text[start:call_offset], base + start))
    count = 0
    for ins in reversed(chunk):
        if ins.mnemonic == "push":
            count += 1
            continue
        if ins.mnemonic in ("mov", "lea", "xor", "test", "cmp", "nop", "and",
                            "or", "shl", "shr", "add", "sub", "movzx", "movsx",
                            "cdq", "int", "cwde", "xchg"):
            continue
        break
    return count


def main():
    path = sys.argv[1]
    out_path = sys.argv[2] if len(sys.argv) > 2 else None

    pe = pefile.PE(path)
    base = pe.OPTIONAL_HEADER.ImageBase
    text_sec = [s for s in pe.sections
                if s.Name.rstrip(b"\x00") == b".text"][0]
    text = text_sec.get_data()
    text_va = base + text_sec.VirtualAddress

    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)

    groups = defaultdict(list)
    for off, disp, reg in scan_call_sites(text, text_va):
        argc = count_pushes_before(md, text, text_va, off)
        groups[(disp, argc)].append((text_va + off, reg))

    lines = []
    lines.append("Indirect vtable calls in %s" % path)
    lines.append("image base 0x%08X, .text at 0x%08X, %d bytes" %
                 (base, text_va, len(text)))
    lines.append("")
    lines.append("Present takes 4 arguments (this + src + dst + dirty), so the")
    lines.append("group of interest is a displacement carrying 4 pushes, ideally")
    lines.append("with its sites clustered in one function rather than spread out.")
    lines.append("")
    lines.append("%-6s %-6s %-6s %-8s %s" %
                 ("disp", "index", "argc", "calls", "sites"))
    lines.append("-" * 104)

    def sort_key(item):
        (disp, argc), sites = item
        return (0 if argc == 4 else 1, -len(sites), disp)

    for (disp, argc), sites in sorted(groups.items(), key=sort_key):
        if argc != 4 and len(sites) < 25:
            continue
        shown = ", ".join("0x%08X" % a for a, _ in sites[:6])
        if len(sites) > 6:
            shown += ", ..."
        lines.append("%-6d %-6d %-6d %-8d %s" %
                     (disp, disp // 4, argc, len(sites), shown))

    text_out = "\n".join(lines) + "\n"
    if out_path:
        with open(out_path, "w", encoding="utf-8") as f:
            f.write(text_out)
    print(text_out)


if __name__ == "__main__":
    main()