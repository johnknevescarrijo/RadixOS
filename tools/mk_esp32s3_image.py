#!/usr/bin/env python3
"""
tools/mk_esp32s3_image.py - ESP32-S3 ROM Bootloader Image Generator & Verifier

Converts a bare-metal Xtensa ELF32 executable into a compact, flashable
ESP32-S3 ROM bootloader binary (without the ~7 MB objcopy DRAM-to-IRAM gap),
with optional 4 MB raw flash image creation (for QEMU/SPI flash) and full
header/checksum/SHA-256 verification.

Zero external dependencies (Python 3 standard library only).
"""

import argparse
import hashlib
import os
import struct
import sys

ESP_IMAGE_HEADER_MAGIC = 0xE9
ESP32S3_CHIP_ID = 0x0009
ESP_CHECKSUM_MAGIC = 0xEF
PT_LOAD = 1
ELF_CLASS_32 = 1
ELF_DATA_LSB = 1

SPI_MODE_DIO = 0x02
# Upper nibble: 2 = 4MB; Lower nibble: 0 = 40MHz (standard safe ROM default)
SPI_SPEED_SIZE_4MB_40MHZ = 0x20


def parse_flash_size(spec: str) -> int:
    s = spec.strip().upper()
    multipliers = {"K": 1024, "KB": 1024, "M": 1024 * 1024, "MB": 1024 * 1024}
    for suffix, mult in sorted(multipliers.items(), key=lambda kv: -len(kv[0])):
        if s.endswith(suffix):
            return int(s[: -len(suffix)]) * mult
    return int(s, 0)


def parse_elf32_segments(elf_path: str):
    with open(elf_path, "rb") as f:
        data = f.read()

    if len(data) < 52 or data[:4] != b"\x7fELF":
        raise ValueError(f"{elf_path}: not a valid ELF file")
    if data[4] != ELF_CLASS_32 or data[5] != ELF_DATA_LSB:
        raise ValueError(f"{elf_path}: expected little-endian ELF32")

    (
        _e_type,
        _e_machine,
        _e_version,
        e_entry,
        e_phoff,
        _e_shoff,
        _e_flags,
        _e_ehsize,
        e_phentsize,
        e_phnum,
    ) = struct.unpack_from("<HHIIIIIHHH", data, 16)

    segments = []
    for idx in range(e_phnum):
        off = e_phoff + idx * e_phentsize
        p_type, p_offset, p_vaddr, p_paddr, p_filesz, _p_memsz, _p_flags, _p_align = (
            struct.unpack_from("<IIIIIIII", data, off)
        )
        if p_type != PT_LOAD or p_filesz == 0:
            continue

        load_addr = p_paddr if p_paddr != 0 else p_vaddr
        payload = data[p_offset : p_offset + p_filesz]
        if len(payload) != p_filesz:
            raise ValueError(f"{elf_path}: truncated PT_LOAD segment {idx}")

        pad_len = (4 - (len(payload) % 4)) % 4
        if pad_len:
            payload += b"\x00" * pad_len

        segments.append((load_addr, payload))

    if not segments:
        raise ValueError(f"{elf_path}: no non-empty PT_LOAD segments found")
    if len(segments) > 16:
        raise ValueError(f"{elf_path}: too many segments ({len(segments)} > 16)")

    return e_entry, segments


def build_esp32s3_image(elf_path: str, out_bin_path: str) -> bytes:
    entry_addr, segments = parse_elf32_segments(elf_path)

    # Standard 8-byte ROM image header
    std_hdr = struct.pack(
        "<BBBBI",
        ESP_IMAGE_HEADER_MAGIC,
        len(segments),
        SPI_MODE_DIO,
        SPI_SPEED_SIZE_4MB_40MHZ,
        entry_addr,
    )

    # Extended 16-byte ESP32-S3 header (total header = 24 bytes)
    ext_hdr = struct.pack(
        "<B3sHB8sB",
        0xEE,          # wp_pin (disabled)
        b"\x00\x00\x00",
        ESP32S3_CHIP_ID,
        0,             # min_chip_rev
        b"\x00" * 8,   # reserved[8]
        1,             # hash_appended = 1
    )

    img = bytearray(std_hdr + ext_hdr)
    checksum = ESP_CHECKSUM_MAGIC

    for load_addr, payload in segments:
        img.extend(struct.pack("<II", load_addr, len(payload)))
        img.extend(payload)
        for b in payload:
            checksum ^= b

    # Pad to 16*k - 1 so adding 1-byte checksum aligns image to 16 bytes
    pad_to_16 = (16 - ((len(img) + 1) % 16)) % 16
    img.extend(b"\x00" * pad_to_16)
    img.append(checksum & 0xFF)

    # Append 32-byte SHA-256 digest over header + segments + padding + checksum
    digest = hashlib.sha256(img).digest()
    img.extend(digest)

    out_dir = os.path.dirname(out_bin_path)
    if out_dir:
        os.makedirs(out_dir, exist_ok=True)
    with open(out_bin_path, "wb") as f:
        f.write(img)

    return bytes(img)


def build_flash_image(app_bin: bytes, flash_path: str, flash_size: int) -> None:
    if len(app_bin) > flash_size:
        raise ValueError(
            f"Application binary ({len(app_bin)} bytes) exceeds flash size ({flash_size} bytes)"
        )
    flash = bytearray(b"\xff" * flash_size)
    # On ESP32-S3, the ROM bootloader loads the primary image from flash offset 0x0000
    flash[0 : len(app_bin)] = app_bin

    out_dir = os.path.dirname(flash_path)
    if out_dir:
        os.makedirs(out_dir, exist_ok=True)
    with open(flash_path, "wb") as f:
        f.write(flash)


def verify_esp32s3_image(bin_path: str) -> dict:
    with open(bin_path, "rb") as f:
        data = f.read()

    if len(data) < 24 + 1 + 32:
        raise ValueError(f"{bin_path}: image too small ({len(data)} bytes)")

    magic, seg_count, spi_mode, spi_speed_size, entry_addr = struct.unpack_from(
        "<BBBBI", data, 0
    )
    if magic != ESP_IMAGE_HEADER_MAGIC:
        raise ValueError(f"{bin_path}: invalid magic 0x{magic:02X} (expected 0xE9)")
    if seg_count == 0 or seg_count > 16:
        raise ValueError(f"{bin_path}: invalid segment count {seg_count}")

    wp_pin, _spi_drv, chip_id, min_rev, _reserved, hash_appended = struct.unpack_from(
        "<B3sHB8sB", data, 8
    )
    if chip_id != ESP32S3_CHIP_ID:
        raise ValueError(
            f"{bin_path}: invalid chip_id 0x{chip_id:04X} (expected 0x0009 for ESP32-S3)"
        )

    offset = 24
    checksum = ESP_CHECKSUM_MAGIC
    segments = []
    for idx in range(seg_count):
        if offset + 8 > len(data):
            raise ValueError(f"{bin_path}: truncated segment {idx} header")
        load_addr, data_len = struct.unpack_from("<II", data, offset)
        offset += 8
        if data_len % 4 != 0:
            raise ValueError(
                f"{bin_path}: segment {idx} length {data_len} is not 4-byte aligned"
            )
        if offset + data_len > len(data):
            raise ValueError(f"{bin_path}: truncated segment {idx} payload")
        payload = data[offset : offset + data_len]
        for b in payload:
            checksum ^= b
        segments.append((load_addr, data_len))
        offset += data_len

    # Remaining bytes up to checksum (at 16-byte boundary - 1)
    pad_len = (16 - ((offset + 1) % 16)) % 16
    checksum_off = offset + pad_len
    if checksum_off >= len(data):
        raise ValueError(f"{bin_path}: missing checksum byte")

    if any(b != 0 for b in data[offset:checksum_off]):
        raise ValueError(f"{bin_path}: non-zero padding before checksum")

    actual_checksum = data[checksum_off]
    if actual_checksum != (checksum & 0xFF):
        raise ValueError(
            f"{bin_path}: checksum mismatch (got 0x{actual_checksum:02X}, expected 0x{checksum & 0xFF:02X})"
        )

    end_off = checksum_off + 1
    sha_ok = False
    if hash_appended == 1:
        if end_off + 32 != len(data):
            raise ValueError(
                f"{bin_path}: invalid length for SHA-256 footer (got {len(data)}, expected {end_off + 32})"
            )
        expected_sha = hashlib.sha256(data[:end_off]).digest()
        actual_sha = data[end_off:]
        if actual_sha != expected_sha:
            raise ValueError(f"{bin_path}: SHA-256 mismatch")
        sha_ok = True
    elif end_off != len(data):
        raise ValueError(f"{bin_path}: trailing bytes after checksum")

    return {
        "size": len(data),
        "entry_addr": entry_addr,
        "chip_id": chip_id,
        "wp_pin": wp_pin,
        "spi_mode": spi_mode,
        "spi_speed_size": spi_speed_size,
        "min_chip_rev": min_rev,
        "segments": segments,
        "checksum": actual_checksum,
        "hash_appended": hash_appended,
        "sha256": data[end_off:].hex() if sha_ok else None,
    }


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Generate and verify ESP32-S3 ROM bootloader images from ELF32."
    )
    parser.add_argument("input_elf", nargs="?", help="Input ELF32 path")
    parser.add_argument("output_bin", nargs="?", help="Output ESP32-S3 .bin path")
    parser.add_argument("--verify", metavar="BIN", help="Verify an existing ESP32-S3 .bin image")
    parser.add_argument(
        "--flash-image",
        metavar="FLASH_BIN",
        help="Also emit a 0xFF-padded SPI flash image with app at 0x0000",
    )
    parser.add_argument(
        "--flash-size",
        default="4M",
        help="Flash size for --flash-image (default: 4M)",
    )
    args = parser.parse_args()

    if args.verify:
        info = verify_esp32s3_image(args.verify)
        print(
            f"[VERIFY OK] {args.verify}: {info['size']} bytes, "
            f"chip_id=0x{info['chip_id']:04X}, entry=0x{info['entry_addr']:08X}, "
            f"segments={len(info['segments'])}, checksum=0x{info['checksum']:02X}, "
            f"sha256={info['sha256'][:16]}..."
        )
        for i, (addr, length) in enumerate(info["segments"]):
            print(f"  segment[{i}]: load_addr=0x{addr:08X}, len={length} bytes")
        return 0

    if not args.input_elf or not args.output_bin:
        parser.error("input_elf and output_bin are required unless --verify is used")

    app_bin = build_esp32s3_image(args.input_elf, args.output_bin)
    info = verify_esp32s3_image(args.output_bin)
    print(
        f"[ESP32S3-IMG] {args.output_bin}: {len(app_bin)} bytes "
        f"({len(info['segments'])} segments, entry=0x{info['entry_addr']:08X}, SHA-256 OK)"
    )

    if args.flash_image:
        flash_size = parse_flash_size(args.flash_size)
        build_flash_image(app_bin, args.flash_image, flash_size)
        print(
            f"[FLASH-IMG] {args.flash_image}: {flash_size} bytes "
            f"(app at 0x0000, remaining 0xFF)"
        )

    return 0


if __name__ == "__main__":
    sys.exit(main())
