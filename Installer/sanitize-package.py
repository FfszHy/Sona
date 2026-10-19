#!/usr/bin/env python3
"""Remove pkgbuild's provenance-only AppleDouble records before product signing.

Recent macOS versions add com.apple.provenance even to cp -X destinations.
pkgbuild serializes it into Payload/Scripts despite COPYFILE_DISABLE. Work on
the raw archives so no signed file, permission, owner or install script changes.
"""

import gzip
from pathlib import Path, PurePosixPath
import struct
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET


def metadata_path(name):
    return any(part.startswith("._") for part in PurePosixPath(name).parts)


def provenance_only(data):
    """Accept only Apple's empty FinderInfo/resource fork plus provenance."""
    if len(data) < 26 or struct.unpack_from(">II", data) != (0x00051607, 0x00020000):
        return False
    count = struct.unpack_from(">H", data, 24)[0]
    if count != 2 or len(data) < 50:
        return False
    entries = [struct.unpack_from(">III", data, 26 + i * 12) for i in range(count)]
    if {entry[0] for entry in entries} != {2, 9}:
        return False
    for kind, start, length in entries:
        if start + length > len(data) or (kind == 2 and length != 0):
            return False
        if kind != 9:
            continue
        # FinderInfo is 32 empty bytes, followed by two padding bytes and ATTR.
        attrs = start + 34
        if length < 70 or data[start:attrs] != bytes(34):
            return False
        header = struct.unpack_from(">8I2H", data, attrs)
        if (header[0] != 0x41545452 or header[2] != len(data)
                or header[3] + header[4] != len(data)
                or any(header[5:9]) or header[9] != 1):
            return False
        entry = attrs + 36
        offset, size, flags, name_size = struct.unpack_from(">IIHB", data, entry)
        name = data[entry + 11:entry + 11 + name_size]
        if (name != b"com.apple.provenance\0" or flags != 0
                or offset != header[3] or size != header[4]
                or offset < entry + 11 + name_size):
            return False
    return True


def clean_cpio(compressed):
    raw = gzip.decompress(compressed)
    offset = 0
    records, paths, removed = [], [], []
    while True:
        begin = offset
        header = raw[offset:offset + 76]
        if len(header) != 76 or header[:6] != b"070707":
            raise ValueError("Expected pkgbuild gzip/ODC cpio archive")
        name_size, size = int(header[59:65], 8), int(header[65:76], 8)
        offset += 76
        name_bytes = raw[offset:offset + name_size]
        if name_size < 1 or name_bytes[-1:] != b"\0":
            raise ValueError("Invalid cpio path")
        name = name_bytes[:-1].decode("utf-8")
        offset += name_size
        data = raw[offset:offset + size]
        offset += size
        if offset > len(raw):
            raise ValueError("Truncated cpio record")
        if metadata_path(name):
            if not provenance_only(data):
                raise ValueError("Unexpected AppleDouble content: " + name)
            removed.append(name)
        else:
            records.append(raw[begin:offset])
            if name != "TRAILER!!!":
                paths.append(name)
        if name == "TRAILER!!!":
            if size or any(raw[offset:]):
                raise ValueError("Unexpected data after cpio trailer")
            break
    cleaned = b"".join(records)
    cleaned += bytes((-len(cleaned)) % 512)
    return gzip.compress(cleaned, mtime=0), paths, removed


def run(args, **kwargs):
    return subprocess.run(args, check=True, **kwargs)


def main(source, destination):
    source, destination = Path(source).resolve(), Path(destination).resolve()
    with tempfile.TemporaryDirectory(prefix="package-metadata-", dir=source.parent) as temp:
        root = Path(temp)
        contents = root / "contents"
        contents.mkdir()
        run(["/usr/bin/xar", "-xf", str(source), "-C", str(contents)])
        members = {path.name for path in contents.iterdir()}
        expected = {"Bom", "PackageInfo", "Payload", "Scripts"}
        if members != expected:
            raise ValueError("Unexpected component package members: " + repr(members))
        removed_total = 0
        for archive in ("Payload", "Scripts"):
            path = contents / archive
            cleaned, paths, removed = clean_cpio(path.read_bytes())
            path.write_bytes(cleaned)
            removed_total += len(removed)
            if archive == "Payload":
                payload_paths = paths

        listing = run(["/usr/bin/lsbom", str(contents / "Bom")],
                      capture_output=True, text=True).stdout
        kept = [line for line in listing.splitlines()
                if not metadata_path(line.split("\t")[0])]
        bom_paths = [line.split("\t")[0] for line in kept]
        if len(bom_paths) != len(payload_paths) or set(bom_paths) != set(payload_paths):
            raise ValueError("Payload and BOM paths differ")
        bom_list = root / "bom.txt"
        bom_list.write_text("\n".join(kept) + "\n")
        run(["/usr/bin/mkbom", "-i", str(bom_list), str(contents / "Bom")])

        info = ET.parse(contents / "PackageInfo")
        payload = info.getroot().find("payload")
        if payload is None:
            raise ValueError("PackageInfo payload is missing")
        payload.set("numberOfFiles", str(len(payload_paths)))
        # Keep pkgbuild's conservative installKBytes estimate. Its native disk
        # accounting is not just the sum of file lengths; removing metadata can
        # only reduce the required space.
        info.write(contents / "PackageInfo", encoding="utf-8", xml_declaration=True)

        # pkgutil --flatten would wrap a raw Scripts archive as one script named
        # Scripts. Rebuild the XAR container directly to retain both raw archives.
        candidate = root / "component.pkg"
        run(["/usr/bin/xar", "--distribution", "--compression=none", "-cf",
             str(candidate), "Bom", "PackageInfo", "Payload", "Scripts"], cwd=contents)
        check = root / "check"
        check.mkdir()
        run(["/usr/bin/xar", "-xf", str(candidate), "-C", str(check)])
        for member in expected:
            if (contents / member).read_bytes() != (check / member).read_bytes():
                raise ValueError("Rebuilt package changed " + member)
        for archive in ("Payload", "Scripts"):
            _, _, removed = clean_cpio((check / archive).read_bytes())
            if removed:
                raise ValueError("AppleDouble remains in " + archive)
        final_bom = run(["/usr/bin/lsbom", "-s", str(check / "Bom")],
                        capture_output=True, text=True).stdout.splitlines()
        if len(final_bom) != len(payload_paths) or set(final_bom) != set(payload_paths):
            raise ValueError("Rebuilt BOM and payload paths differ")
        final_listing = run(["/usr/bin/lsbom", str(check / "Bom")],
                            capture_output=True, text=True).stdout.splitlines()
        if set(final_listing) != set(kept):
            raise ValueError("Rebuilt BOM changed file attributes")
        candidate.replace(destination)
        print(f"   removed {removed_total} provenance metadata records; "
              f"{len(payload_paths)} payload entries verified")


if __name__ == "__main__":
    if len(sys.argv) != 3:
        raise SystemExit("usage: sanitize-package.py source.pkg destination.pkg")
    main(*sys.argv[1:])
