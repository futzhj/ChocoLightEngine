#!/usr/bin/env python3
"""ChocoLight Pack Tool — Pack Lua scripts into template exe.

Usage:
    python lightpack.py <template.exe> <script.lua> -o <output.exe> [-k xor_key]

Flow:
    1. Compile .lua → .luac via lightc
    2. Generate random opcode permutation (38 opcodes)
    3. Remap bytecode opcodes using permutation
    4. XOR-encrypt remapped bytecode
    5. Build Pack Header (perm + key encrypted with master_key)
    6. Append payload + header to template exe copy
"""
import sys, os, struct, shutil, subprocess, random, argparse

# ============================================================
# Master key — obfuscated storage (XOR with index)
# Actual key: "ChocoLight2026!@#"
# ============================================================
_MK_ENC = bytes([
    0x43^0, 0x68^1, 0x6F^2, 0x63^3, 0x6F^4, 0x4C^5, 0x69^6, 0x67^7,
    0x68^8, 0x74^9, 0x32^10, 0x30^11, 0x32^12, 0x36^13, 0x21^14, 0x40^15,
    0x23^16
])

def _get_master_key():
    """Recover master key from obfuscated storage"""
    return bytes(b ^ i for i, b in enumerate(_MK_ENC))

# ============================================================
# Pack Header format (128 bytes, little-endian)
# ============================================================
PACK_MAGIC = b"LMPK"
PACK_VERSION = 1
HEADER_SIZE = 128
OPCODE_COUNT = 38
XOR_KEY_MAX = 64

def build_header(payload_size, perm_table, xor_key, master_key):
    """Build 128-byte pack header with encrypted fields"""
    # Encrypt perm_table with master_key
    mk = master_key
    enc_perm = bytes(perm_table[i] ^ mk[i % len(mk)] for i in range(OPCODE_COUNT))

    # Encrypt xor_key with master_key
    xk_bytes = xor_key.encode('utf-8') if isinstance(xor_key, str) else xor_key
    xk_len = min(len(xk_bytes), XOR_KEY_MAX)
    enc_xk = bytes(xk_bytes[i] ^ mk[i % len(mk)] for i in range(xk_len))
    enc_xk = enc_xk.ljust(XOR_KEY_MAX, b'\x00')

    header = bytearray(HEADER_SIZE)
    # magic (4) + version (4) + payload_size (4) + flags (4)
    struct.pack_into('<4sIII', header, 0, PACK_MAGIC, PACK_VERSION, payload_size, 0)
    # perm_table (38 bytes @ offset 16)
    header[16:16+OPCODE_COUNT] = enc_perm
    # xor_key_len (2 bytes @ offset 54)
    struct.pack_into('<H', header, 54, xk_len)
    # xor_key (64 bytes @ offset 56)
    header[56:56+XOR_KEY_MAX] = enc_xk
    # reserved (8 bytes @ offset 120) — zero
    return bytes(header)

def parse_header(data, master_key):
    """Parse and decrypt pack header. Returns (payload_size, perm, xor_key) or None"""
    if len(data) < HEADER_SIZE:
        return None
    magic, ver, payload_size, flags = struct.unpack_from('<4sIII', data, 0)
    if magic != PACK_MAGIC or ver != PACK_VERSION:
        return None

    mk = master_key
    # Decrypt perm table
    enc_perm = data[16:16+OPCODE_COUNT]
    perm = [enc_perm[i] ^ mk[i % len(mk)] for i in range(OPCODE_COUNT)]

    # Decrypt XOR key
    xk_len = struct.unpack_from('<H', data, 54)[0]
    enc_xk = data[56:56+xk_len]
    xor_key = bytes(enc_xk[i] ^ mk[i % len(mk)] for i in range(xk_len))

    return payload_size, perm, xor_key

# ============================================================
# Lua 5.1 Bytecode Opcode Remapping
# ============================================================
# Instruction format: 32-bit, opcode in bits [0:5] (6 bits)
OPCODE_MASK = 0x3F  # 6 bits

def remap_opcodes_in_bytecode(data, perm):
    """Remap opcode fields in Lua 5.1 bytecode.

    Lua 5.1 .luac format:
    - 12-byte header (magic + version + format + endian + sizes)
    - Then function prototype (recursive)
    
    We need to find all instruction arrays and remap their opcode fields.
    """
    buf = bytearray(data)
    pos = [0]  # mutable position counter

    def read_byte():
        b = buf[pos[0]]
        pos[0] += 1
        return b

    def read_int():
        val = struct.unpack_from('<i', buf, pos[0])[0]
        pos[0] += 4
        return val

    def read_uint():
        val = struct.unpack_from('<I', buf, pos[0])[0]
        pos[0] += 4
        return val

    def read_number():
        val = struct.unpack_from('<d', buf, pos[0])[0]
        pos[0] += 8
        return val

    def read_string():
        # Size-prefixed string (size_t = 8 bytes on x64, 4 on x86)
        sz = struct.unpack_from(f'<{size_t_fmt}', buf, pos[0])[0]
        pos[0] += size_t_size
        if sz == 0:
            return b''
        s = buf[pos[0]:pos[0]+sz]
        pos[0] += sz
        return s

    def remap_function():
        """Parse and remap opcodes in one function prototype"""
        read_string()  # source name
        read_int()     # line defined
        read_int()     # last line defined
        read_byte()    # nups
        read_byte()    # numparams
        read_byte()    # is_vararg
        read_byte()    # maxstacksize

        # Instructions — this is where we remap
        num_instructions = read_int()
        instr_start = pos[0]
        for i in range(num_instructions):
            offset = instr_start + i * 4
            instr = struct.unpack_from('<I', buf, offset)[0]
            old_op = instr & OPCODE_MASK
            if old_op < OPCODE_COUNT:
                new_op = perm[old_op]
                instr = (instr & ~OPCODE_MASK) | new_op
                struct.pack_into('<I', buf, offset, instr)
        pos[0] = instr_start + num_instructions * 4

        # Constants
        num_constants = read_int()
        for _ in range(num_constants):
            t = read_byte()
            if t == 0:    # nil
                pass
            elif t == 1:  # boolean
                read_byte()
            elif t == 3:  # number
                read_number()
            elif t == 4:  # string
                read_string()

        # Inner function prototypes (recursive)
        num_protos = read_int()
        for _ in range(num_protos):
            remap_function()

        # Debug info — skip through
        # Source line positions
        num_lineinfo = read_int()
        pos[0] += num_lineinfo * 4
        # Local variables
        num_locals = read_int()
        for _ in range(num_locals):
            read_string()
            read_int()
            read_int()
        # Upvalue names
        num_upvalues = read_int()
        for _ in range(num_upvalues):
            read_string()

    # ---- Parse .luac header ----
    # Signature: 4 bytes (\033Lua)
    sig = buf[0:4]
    if sig != b'\x1bLua':
        raise ValueError(f"Not a valid Lua bytecode file (sig={sig.hex()})")

    version = buf[4]      # 0x51 for Lua 5.1
    fmt = buf[5]           # 0 = official
    endian = buf[6]        # 1 = little-endian
    int_size = buf[7]      # usually 4
    size_t_byte = buf[8]   # 4 or 8
    instr_size = buf[9]    # 4
    number_size = buf[10]  # 8
    integral = buf[11]     # 0 = floating point

    # Determine size_t format
    size_t_size = size_t_byte
    size_t_fmt = 'Q' if size_t_size == 8 else 'I'

    pos[0] = 12  # skip header
    remap_function()

    return bytes(buf)

# ============================================================
# XOR encryption
# ============================================================
def xor_encrypt(data, key):
    """XOR encrypt/decrypt data with key"""
    kb = key.encode('utf-8') if isinstance(key, str) else key
    return bytes(data[i] ^ kb[i % len(kb)] for i in range(len(data)))

# ============================================================
# Main packing flow
# ============================================================
def pack(template_exe, lua_file, output_exe, xor_key, lightc_path=None):
    """Pack a Lua script into a template exe"""
    mk = _get_master_key()

    # Step 1: Compile .lua → .luac
    if lightc_path is None:
        # Try to find lightc next to this script
        script_dir = os.path.dirname(os.path.abspath(__file__))
        candidates = [
            os.path.join(script_dir, 'lightc.exe'),
            os.path.join(script_dir, '..', 'build', 'src', 'lightc', 'Release', 'lightc.exe'),
        ]
        for c in candidates:
            if os.path.isfile(c):
                lightc_path = c
                break

    luac_file = lua_file + 'c'  # main.luac
    if lightc_path and os.path.isfile(lightc_path):
        print(f"[lightpack] Compiling: {lua_file} → {luac_file}")
        ret = subprocess.run([lightc_path, '-o', luac_file, lua_file],
                             capture_output=True, text=True)
        if ret.returncode != 0:
            print(f"[lightpack] lightc error: {ret.stderr}")
            sys.exit(1)
    else:
        # Fallback: use luac or embed source directly
        print(f"[lightpack] Warning: lightc not found, embedding source as-is")
        # For source embedding, we skip opcode remapping
        with open(lua_file, 'rb') as f:
            payload = f.read()
        payload = xor_encrypt(payload, xor_key)
        # Use identity permutation (no remap for source)
        perm = list(range(OPCODE_COUNT))
        _finalize(template_exe, output_exe, payload, perm, xor_key, mk)
        return

    # Step 2: Read compiled bytecode
    with open(luac_file, 'rb') as f:
        bytecode = f.read()

    # Step 3: Generate random opcode permutation
    perm = list(range(OPCODE_COUNT))
    random.shuffle(perm)
    print(f"[lightpack] Opcode permutation generated (seed={os.urandom(4).hex()})")

    # Step 4: Remap bytecode opcodes
    remapped = remap_opcodes_in_bytecode(bytecode, perm)
    print(f"[lightpack] Bytecode remapped ({len(remapped)} bytes)")

    # Step 5: XOR encrypt
    payload = xor_encrypt(remapped, xor_key)

    # Step 6: Finalize
    _finalize(template_exe, output_exe, payload, perm, xor_key, mk)

    # Cleanup temp .luac
    if os.path.isfile(luac_file):
        os.remove(luac_file)

def _finalize(template_exe, output_exe, payload, perm, xor_key, mk):
    """Copy template + append payload + header"""
    header = build_header(len(payload), perm, xor_key, mk)

    shutil.copy2(template_exe, output_exe)
    with open(output_exe, 'ab') as f:
        f.write(payload)
        f.write(header)

    total = os.path.getsize(output_exe)
    template_size = os.path.getsize(template_exe)
    print(f"[lightpack] Packed: {output_exe}")
    print(f"  Template: {template_size:,} bytes")
    print(f"  Payload:  {len(payload):,} bytes (encrypted)")
    print(f"  Header:   {HEADER_SIZE} bytes")
    print(f"  Total:    {total:,} bytes")

# ============================================================
# CLI
# ============================================================
def main():
    parser = argparse.ArgumentParser(
        description='ChocoLight Pack Tool — Pack Lua scripts into template exe')
    parser.add_argument('template', help='Template exe path (e.g., template_lightw.exe)')
    parser.add_argument('script', help='Lua script to pack (e.g., main.lua)')
    parser.add_argument('-o', '--output', required=True, help='Output exe path')
    parser.add_argument('-k', '--key', default='ChocoLight2026',
                        help='XOR encryption key (default: ChocoLight2026)')
    parser.add_argument('-c', '--lightc', default=None,
                        help='Path to lightc.exe compiler')
    args = parser.parse_args()

    if not os.path.isfile(args.template):
        print(f"Error: template not found: {args.template}")
        sys.exit(1)
    if not os.path.isfile(args.script):
        print(f"Error: script not found: {args.script}")
        sys.exit(1)

    pack(args.template, args.script, args.output, args.key, args.lightc)

if __name__ == '__main__':
    main()
