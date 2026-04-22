# /// script
# requires-python = ">=3.10"
# dependencies = ["pefile"]
# ///
"""
patch_jubeat.py — append-only patcher for jubeat.dll.

Rewrites jubeat.dll so tutorial_fsm and GFAVSSwapBuffers calls go
through tutorial_skip.dll's hooks at load time. No runtime memory
mutation needed.

STRICTLY APPEND-ONLY: existing import/export/IAT regions and section
contents are not relocated. New structures live in a single appended
`.skip` section. The only in-place edit is the `.text` patch for the
two call-site rewrites.
"""

import sys
import struct

import pefile

# Must stay in sync with src/hook.cpp.
TUTORIAL_FSM_PATTERN = bytes.fromhex(
    "8B430C"        # mov eax, [ebx+0Ch]   ; msg_id
    "8B7310"        # mov esi, [ebx+10h]   ; request_ptr
    "8B7B08"        # mov edi, [ebx+08h]   ; fsm
    "89B53CFFFFFF"  # mov [ebp-0C4h], esi
    "3D01000080"    # cmp eax, 80000001h   ; MSG_CREATE?
    "0F85"          # jne state_cases       (rel32 disp varies between builds)
)
TUTORIAL_FSM_SIG_OFFSET = 0x41

SKIPPER_DLL  = b"tutorial_skip.dll\x00"
HOOK_FSM     = b"hook_tutorial_fsm\x00"
HOOK_SWAP    = b"hook_GFAVSSwapBuffers\x00"
GFTOOLS_DLL  = "gftools.dll"
GFAVS_NAME   = "GFAVSSwapBuffers"

IID_FMT  = "<IIIII"   # IMAGE_IMPORT_DESCRIPTOR (5 dwords, 20 bytes)
IID_SIZE = 20
IED_FMT  = "<IIHHIIIIIII"  # IMAGE_EXPORT_DIRECTORY (40 bytes)
IED_SIZE = 40

SECT_CODE  = 0x00000020
SECT_IDATA = 0x00000040
SECT_EXEC  = 0x20000000
SECT_READ  = 0x40000000
SECT_WRITE = 0x80000000


def align_up(value, alignment):
    return (value + alignment - 1) & ~(alignment - 1)


def find_pattern(buf, pattern):
    return [i for i in range(len(buf) - len(pattern) + 1)
            if buf[i:i + len(pattern)] == pattern]


def find_iat_slot_rva(pe, dll, fn):
    for entry in pe.DIRECTORY_ENTRY_IMPORT:
        if entry.dll.decode().lower() != dll.lower():
            continue
        for imp in entry.imports:
            if imp.name and imp.name.decode() == fn:
                return imp.address - pe.OPTIONAL_HEADER.ImageBase
    raise KeyError(f"{dll}!{fn} not in imports")


def rewrite_direct_calls(buf, text_va, target_va, new_target_va):
    n = 0
    i = 0
    while i < len(buf) - 5:
        if buf[i] == 0xE8:
            rel = struct.unpack_from("<i", buf, i + 1)[0]
            if ((text_va + i + 5 + rel) & 0xFFFFFFFF) == (target_va & 0xFFFFFFFF):
                struct.pack_into("<i", buf, i + 1,
                                 new_target_va - (text_va + i + 5))
                n += 1
                i += 5
                continue
        i += 1
    return n


# tutorial_fsm is registered as an FSM callback by address rather than
# called directly: the compiler emits `push imm32` / `mov reg, imm32`
# with the function VA inlined. We rewrite every 4-byte literal that
# matches the function VA to point at our indirect-call thunk instead.
# Collisions with unrelated 4-byte values that happen to equal a code
# VA are vanishingly unlikely in `.text`.
def rewrite_addr_literals(buf, target_va, new_target_va):
    needle = struct.pack("<I", target_va & 0xFFFFFFFF)
    repl   = struct.pack("<I", new_target_va & 0xFFFFFFFF)
    n = 0
    i = 0
    while i <= len(buf) - 4:
        if buf[i:i + 4] == needle:
            buf[i:i + 4] = repl
            n += 1
            i += 4
        else:
            i += 1
    return n


def rewrite_indirect_calls(buf, old_iat_va, new_iat_va):
    n = 0
    i = 0
    while i < len(buf) - 6:
        if buf[i] == 0xFF and buf[i + 1] == 0x15:
            if struct.unpack_from("<I", buf, i + 2)[0] == old_iat_va:
                struct.pack_into("<I", buf, i + 2, new_iat_va)
                n += 1
                i += 6
                continue
        i += 1
    return n


def main(in_path, out_path):
    pe = pefile.PE(in_path, fast_load=False)
    base = pe.OPTIONAL_HEADER.ImageBase
    file_align = pe.OPTIONAL_HEADER.FileAlignment
    sect_align = pe.OPTIONAL_HEADER.SectionAlignment

    # Stash the Authenticode certificate (if any) so we can detach it across
    # the patch and re-attach at the new file end. The SECURITY directory's
    # VirtualAddress is a *file offset*, not an RVA.
    security_dir = pe.OPTIONAL_HEADER.DATA_DIRECTORY[4]
    cert_off = security_dir.VirtualAddress
    cert_size = security_dir.Size
    has_cert = cert_off != 0 and cert_size != 0
    cert_bytes = b""
    if has_cert:
        with open(in_path, 'rb') as f:
            f.seek(cert_off)
            cert_bytes = f.read(cert_size)

    # ---- 1. locate tutorial_fsm in .text ----
    text = next(s for s in pe.sections if s.Name.startswith(b'.text'))
    text_bytes = bytearray(text.get_data())
    hits = find_pattern(bytes(text_bytes), TUTORIAL_FSM_PATTERN)
    if len(hits) != 1:
        sys.exit(f"tutorial_fsm pattern matched {len(hits)} times")
    fsm_rva = text.VirtualAddress + hits[0] - TUTORIAL_FSM_SIG_OFFSET
    fsm_va  = base + fsm_rva
    text_va = base + text.VirtualAddress
    print(f"[patcher] tutorial_fsm @ VA 0x{fsm_va:08x}")

    # ---- 2. find existing GFAVSSwapBuffers IAT slot ----
    old_swap_iat_rva = find_iat_slot_rva(pe, GFTOOLS_DLL, GFAVS_NAME)
    old_swap_iat_va  = base + old_swap_iat_rva
    print(f"[patcher] old GFAVSSwapBuffers IAT @ VA 0x{old_swap_iat_va:08x}")

    # ---- 3. plan two appended sections ----
    # `.skip`  — R+W data (import/export tables, IAT, strings).
    # `.skipt` — R+X thunk (single 6-byte `jmp [hook_fsm_iat]`).
    # Splitting keeps `.skip` non-executable and the IAT writable for the
    # loader, while the FSM-callback indirection still has somewhere to land.
    last = pe.sections[-1]
    skip_rva = align_up(last.VirtualAddress + last.Misc_VirtualSize, sect_align)
    skip_raw = align_up(last.PointerToRawData + last.SizeOfRawData, file_align)

    # Layout of `.skip` (offsets relative to its start):
    n_old_imports = len(pe.DIRECTORY_ENTRY_IMPORT)
    iids_off = 0
    iids_size = (n_old_imports + 2) * IID_SIZE  # old + 1 new + null
    int_off  = iids_size
    int_size = 3 * 4                            # 2 entries + null
    iat_off  = int_off + int_size
    iat_size = 3 * 4
    hint_off = iat_off + iat_size

    hints = bytearray()
    fsm_hint  = len(hints); hints += b'\x00\x00' + HOOK_FSM
    if len(hints) & 1: hints += b'\x00'
    swap_hint = len(hints); hints += b'\x00\x00' + HOOK_SWAP
    if len(hints) & 1: hints += b'\x00'

    dll_name_off = hint_off + len(hints)

    new_fsm_iat_rva  = skip_rva + iat_off
    new_swap_iat_rva = skip_rva + iat_off + 4

    if hasattr(pe, 'DIRECTORY_ENTRY_EXPORT') and pe.DIRECTORY_ENTRY_EXPORT:
        ex = pe.DIRECTORY_ENTRY_EXPORT
        old_funcs = [(s.address, s.name, s.ordinal) for s in ex.symbols]
        export_base = ex.struct.Base
    else:
        old_funcs = []
        export_base = 1

    new_funcs = old_funcs + [
        (fsm_rva, b"tutorial_fsm", export_base + len(old_funcs)),
    ]
    n_funcs = len(new_funcs)
    n_names = sum(1 for _, n, _ in new_funcs if n)

    edir_off       = align_up(dll_name_off + len(SKIPPER_DLL), 4)
    funcs_off      = edir_off + IED_SIZE
    names_off      = funcs_off + n_funcs * 4
    ords_off       = names_off + n_names * 4
    estr_off       = ords_off + n_names * 2

    estrings = bytearray()
    name_str_off = {}
    for _, name, _ in new_funcs:
        if name:
            name_str_off[name] = len(estrings)
            estrings += name + b'\x00'
    dll_export_name_str_off = len(estrings)
    estrings += b"jubeat.dll\x00"

    # ---- 4. assemble the blob ----
    blob = bytearray()

    for entry in pe.DIRECTORY_ENTRY_IMPORT:
        s = entry.struct
        blob += struct.pack(IID_FMT,
            s.OriginalFirstThunk, s.TimeDateStamp, s.ForwarderChain,
            s.Name, s.FirstThunk)
    blob += struct.pack(IID_FMT,
        skip_rva + int_off,
        0, 0,
        skip_rva + dll_name_off,
        skip_rva + iat_off)
    blob += b'\x00' * IID_SIZE

    blob += struct.pack("<III",
        skip_rva + hint_off + fsm_hint,
        skip_rva + hint_off + swap_hint, 0)
    blob += struct.pack("<III",
        skip_rva + hint_off + fsm_hint,
        skip_rva + hint_off + swap_hint, 0)
    blob += hints
    blob += SKIPPER_DLL
    while len(blob) < edir_off:
        blob += b'\x00'

    blob += struct.pack(IED_FMT,
        0, 0, 0, 0,
        skip_rva + estr_off + dll_export_name_str_off,
        export_base,
        n_funcs, n_names,
        skip_rva + funcs_off,
        skip_rva + names_off,
        skip_rva + ords_off)

    funcs_array = [0] * n_funcs
    for addr, _, ordinal in new_funcs:
        funcs_array[ordinal - export_base] = addr
    for a in funcs_array:
        blob += struct.pack("<I", a)

    named = sorted([f for f in new_funcs if f[1]], key=lambda f: f[1])
    for _, name, _ in named:
        blob += struct.pack("<I", skip_rva + estr_off + name_str_off[name])
    for _, _, ordinal in named:
        blob += struct.pack("<H", ordinal - export_base)

    blob += estrings

    edir_size = len(blob) - edir_off

    skip_vsize = len(blob)
    skip_raw_size = align_up(skip_vsize, file_align)

    # `.skipt` lives directly after `.skip`, holding only the thunk.
    skipt_rva = align_up(skip_rva + skip_vsize, sect_align)
    skipt_raw = align_up(skip_raw + skip_raw_size, file_align)
    thunk_va  = base + skipt_rva
    thunk_blob = b"\xFF\x25" + struct.pack("<I", base + new_fsm_iat_rva)
    skipt_vsize    = len(thunk_blob)
    skipt_raw_size = align_up(skipt_vsize, file_align)

    # The thunk's `FF 25 imm32` encodes the IAT slot's absolute VA. When the
    # image loads at a non-preferred base the loader needs a relocation entry
    # to fix up that imm32, otherwise the thunk dereferences a stale VA in
    # some other module's address space and jumps to garbage.
    IMAGE_REL_BASED_HIGHLOW = 3
    thunk_imm_rva = skipt_rva + 2
    page_rva = thunk_imm_rva & ~0xFFF
    extra_reloc = struct.pack("<IIHH",
        page_rva, 12,
        (IMAGE_REL_BASED_HIGHLOW << 12) | (thunk_imm_rva & 0xFFF),
        0,
    )
    reloc_sect = next(s for s in pe.sections if s.Name.startswith(b'.reloc'))
    old_reloc_size = pe.OPTIONAL_HEADER.DATA_DIRECTORY[5].Size
    if old_reloc_size + len(extra_reloc) > reloc_sect.SizeOfRawData:
        sys.exit("no padding room in .reloc to append a relocation block")

    # ---- 5. patch .text in place ----
    n_d = rewrite_direct_calls(text_bytes, text_va, fsm_va, thunk_va)
    n_a = rewrite_addr_literals(text_bytes, fsm_va, thunk_va)
    n_i = rewrite_indirect_calls(text_bytes, old_swap_iat_va, base + new_swap_iat_rva)
    print(f"[patcher] rewrote {n_d} direct + {n_a} addr-of + {n_i} indirect call(s)")
    if n_d + n_a == 0:
        sys.exit("found no tutorial_fsm references in .text")
    if n_i == 0:
        sys.exit("found no GFAVSSwapBuffers indirect calls in .text")

    pe.set_bytes_at_offset(text.PointerToRawData, bytes(text_bytes))

    # ---- 6. compute section header location and bail early if it doesn't fit ----
    e_lfanew = pe.DOS_HEADER.e_lfanew
    file_header_off = e_lfanew + 4
    opt_header_off  = file_header_off + 20
    size_opt        = pe.FILE_HEADER.SizeOfOptionalHeader
    sect_table_off  = opt_header_off + size_opt
    n_sect          = pe.FILE_HEADER.NumberOfSections
    skip_hdr_off    = sect_table_off + n_sect * 0x28
    skipt_hdr_off   = skip_hdr_off + 0x28
    first_sect_raw = min(s.PointerToRawData for s in pe.sections if s.PointerToRawData > 0)
    if skipt_hdr_off + 0x28 > first_sect_raw:
        sys.exit("no room in section table for two new entries")

    is_pe32_plus = pe.OPTIONAL_HEADER.Magic == 0x20B
    data_dir_off = opt_header_off + (112 if is_pe32_plus else 96)

    # ---- 7. let pefile serialize text edits + opt-header changes ----
    # We point the IMPORT/EXPORT directories at the new tables here so pefile
    # writes them into the optional header. The raw bytes for the new section
    # header and `.skip` payload are then patched in directly. The SECURITY
    # directory is zeroed for now — we'll re-attach the cert at the new file
    # end and re-point it once everything else is laid out.
    pe.OPTIONAL_HEADER.DATA_DIRECTORY[0].VirtualAddress = skip_rva + edir_off
    pe.OPTIONAL_HEADER.DATA_DIRECTORY[0].Size = edir_size
    pe.OPTIONAL_HEADER.DATA_DIRECTORY[1].VirtualAddress = skip_rva + iids_off
    pe.OPTIONAL_HEADER.DATA_DIRECTORY[1].Size = iids_size
    pe.OPTIONAL_HEADER.DATA_DIRECTORY[4].VirtualAddress = 0
    pe.OPTIONAL_HEADER.DATA_DIRECTORY[4].Size = 0
    # DATA_DIRECTORY[12] (IAT) stays at the original IAT region — it's only a
    # hint for the loader, which works off the import descriptors.
    pe.OPTIONAL_HEADER.DATA_DIRECTORY[5].Size = old_reloc_size + len(extra_reloc)
    pe.OPTIONAL_HEADER.SizeOfImage = align_up(skipt_rva + skipt_vsize, sect_align)
    pe.write(out_path)

    # ---- 8. write the new section headers + payloads directly ----
    skip_header = struct.pack(
        "<8sIIIIIIHHI",
        b'.skip\x00\x00\x00',
        skip_vsize, skip_rva, skip_raw_size, skip_raw,
        0, 0, 0, 0,
        SECT_IDATA | SECT_READ | SECT_WRITE,
    )
    skipt_header = struct.pack(
        "<8sIIIIIIHHI",
        b'.skipt\x00\x00',
        skipt_vsize, skipt_rva, skipt_raw_size, skipt_raw,
        0, 0, 0, 0,
        SECT_CODE | SECT_READ | SECT_EXEC,
    )

    with open(out_path, 'r+b') as f:
        f.seek(skip_hdr_off)
        f.write(skip_header)
        f.write(skipt_header)
        f.seek(file_header_off + 2)
        f.write(struct.pack("<H", n_sect + 2))
        # Append the new reloc block into .reloc's trailing padding and
        # bump the section's VirtualSize to match.
        f.seek(reloc_sect.PointerToRawData + old_reloc_size)
        f.write(extra_reloc)
        reloc_sect_hdr_off = sect_table_off + (
            list(pe.sections).index(reloc_sect) * 0x28)
        f.seek(reloc_sect_hdr_off + 8)  # Misc/VirtualSize
        f.write(struct.pack("<I", reloc_sect.Misc_VirtualSize + len(extra_reloc)))
        # Drop any trailing cert bytes pefile carried over from the input.
        if has_cert:
            f.truncate(cert_off)
        f.seek(0, 2)
        cur_size = f.tell()
        if cur_size < skip_raw:
            f.write(b'\x00' * (skip_raw - cur_size))
        f.seek(skip_raw)
        f.write(bytes(blob) + b'\x00' * (skip_raw_size - len(blob)))
        f.seek(skipt_raw)
        f.write(thunk_blob + b'\x00' * (skipt_raw_size - len(thunk_blob)))

        if has_cert:
            f.seek(0, 2)
            pad = (-f.tell()) & 7  # Authenticode requires 8-byte alignment
            f.write(b'\x00' * pad)
            new_cert_off = f.tell()
            f.write(cert_bytes)
            security_dd_off = data_dir_off + 4 * 8
            f.seek(security_dd_off)
            f.write(struct.pack("<II", new_cert_off, cert_size))

    print(f"[patcher] wrote {out_path}")


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit("usage: patch_jubeat.py <input.dll> <output.dll>")
    main(sys.argv[1], sys.argv[2])
