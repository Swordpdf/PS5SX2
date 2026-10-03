#!/usr/bin/env python3
"""Generate link-only AGC import metadata from genstub C symbol listings.

These files contain no library implementation. The native app builder uses
their exported names and SONAME to import the console's system modules.
Console symbol availability needs proper testing. (AI-assisted)
"""

import argparse
from pathlib import Path
import re
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("symbol_dir", type=Path)
    parser.add_argument("output_dir", type=Path)
    parser.add_argument("--clang", default="clang")
    parser.add_argument("--lld", default="ld.lld")
    args = parser.parse_args()

    # Validate both inputs before writing any outputs; never execute the C files.
    modules = {}
    for module in ("libSceAgc", "libSceAgcDriver"):
        source = (args.symbol_dir / (module + ".c")).read_text()
        symbols = sorted(set(re.findall(r"\.global\s+(sceAgc[A-Za-z0-9_]+)\\n", source)))
        if not symbols:
            parser.error(f"No AGC export declarations found in {module}.c")
        modules[module] = symbols

    args.output_dir.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="ps5-agc-imports-") as temporary:
        work = Path(temporary)
        for module, symbols in modules.items():
            assembly = work / (module + ".s")
            obj = work / (module + ".o")
            assembly.write_text(".text\n" + "".join(
                f".global {symbol}\n.type {symbol}, @function\n{symbol}:\n"
                for symbol in symbols
            ) + '.section .note.GNU-stack,"",@progbits\n')
            subprocess.run([args.clang, "--target=x86_64-sie-ps5", "-c",
                            str(assembly), "-o", str(obj)], check=True)
            output = args.output_dir / (module + ".so")
            subprocess.run([args.lld, "-m", "elf_x86_64", "-shared",
                            "-soname", module + ".sprx", "-o", str(output),
                            str(obj)], check=True)
            print(f"{output}: {len(symbols)} import symbols; not a runtime library")


if __name__ == "__main__":
    main()
