#!/usr/bin/env python3
"""Make the 2012 static ntuplace3 binary run on kernels without the vsyscall page.

The binary's glibc calls gettimeofday() and time() through the legacy vsyscall
page (0xffffffffff600000), which kernels booted with vsyscall=none, WSL2's
among them, do not map: ntuplace3 dies with SIGSEGV as it starts timing its
first QP solve. Every such call is the same 9 bytes,

    mov $0xffffffffff6000NN,%rax ; call *%rax

and is rewritten in place to the system call it stood for,

    mov $NR,%eax ; syscall ; nop ; nop

with the arguments already in %rdi and %rsi. The input is not modified.

Usage: patch_ntuplace3.py <ntuplace3> <patched output>
"""

import pathlib
import sys

CALLS = {
    0x000: ("gettimeofday", 96),
    0x400: ("time", 201),
}


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__.strip().splitlines()[-1], file=sys.stderr)
        return 2
    source, target = map(pathlib.Path, sys.argv[1:])
    data = bytearray(source.read_bytes())
    patched = 0
    for slot, (name, number) in CALLS.items():
        pattern = b"\x48\xc7\xc0" + (0xFF600000 + slot).to_bytes(4, "little") + b"\xff\xd0"
        replacement = b"\xb8" + number.to_bytes(4, "little") + b"\x0f\x05\x90\x90"
        count = data.count(pattern)
        data = data.replace(pattern, replacement)
        patched += count
        print(f"{name}: {count} call(s) rewritten")
    if patched == 0:
        print("no vsyscall calls found; is this the ntuplace3 binary?", file=sys.stderr)
        return 1
    target.write_bytes(data)
    target.chmod(0o755)
    print(f"wrote {target}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
