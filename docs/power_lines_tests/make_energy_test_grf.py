import struct, sys
def pseudo(b): return struct.pack("<IB", len(b), 0xFF) + b
def node_c(label, children): return b"C" + label + b"".join(children) + b"\x00"
def node_t(label, text): return b"T" + label + b"\x7f" + text + b"\x00"
def node_b(label, data): return b"B" + label + struct.pack("<H", len(data)) + data
def make(path, feature_name, grfid):
    sprites = []
    ftst = node_c(b"FTST", [node_t(b"NAME", feature_name), node_b(b"MINV", struct.pack("<H", 1)), node_b(b"SETP", b"\x10")])
    sprites.append(b"\x14" + node_c(b"INFO", [node_t(b"NAME", b"Energy test")]) + ftst + b"\x00")
    sprites.append(b"\x08\x08" + grfid + b"Energy test\x00Tests fork-only energy properties\x00")
    # Skip the energy properties when parameter 0x10 is not 1.
    sprites.append(b"\x07\x10\x01\x03\x01\x02")
    # Industry 0: a copy of the power station, a fuel plant of 1000 MW burning coal at 5 MW-months a tonne.
    sprites.append(b"\x00\x0a\x04\x01\x00" + b"\x08\x01" + b"\x60\x02" + b"\x61" + struct.pack("<H", 10000) + b"\x64\x01COAL" + struct.pack("<H", 50))
    # House 0: a copy of house 0, low under wires, 5 MW idle.
    sprites.append(b"\x00\x07\x03\x01\x00" + b"\x08\x00" + b"\x60\x32" + b"\x61\x01")
    sprites.insert(0, struct.pack("<I", len(sprites)))
    data = b"".join(pseudo(s) for s in sprites) + struct.pack("<I", 0)
    header = b"\x00\x00GRF\x82\x0d\x0a\x1a\x0a"
    with open(path, "wb") as f:
        f.write(header + struct.pack("<I", 1 + len(data)) + b"\x00" + data + struct.pack("<I", 0))
make(sys.argv[1], b"energy_transport", b"ET\x01\x01")
make(sys.argv[2], b"no_such_feature", b"ET\x01\x02")
