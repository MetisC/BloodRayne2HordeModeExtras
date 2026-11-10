"""Build MODELS\BR2_DHAMPIR_BLADES.SMB - a copy of the stock
WEAPON_DHAMPIR_BLADES.SMB whose collision parts are renamed to the Rayne
player-weapon convention.

CAnimatedMeleeWeapon (player weapon type 0) looks up parts named
'norm_collision', 'rage_collision1' and 'fury_collision2' inside the blade
model.  The stock dhampir blade names the same three collision volumes
'collision', 'collision1' and 'collision2', which makes the lookup fail
fatally ("Can't find part norm_collision ...").  All visual blade parts
(norm_blade_*, rage_blade_*, fury_blade_*) already match Rayne's naming,
so only the three collision names need renaming.

Part-name fields in .SMB are fixed 16-byte slots; all new names fit.

A distinct filename (br2_dhampir_blades.smb) keeps the stock model intact
for enemy CAnimatedMeleeWeapon instances that use type-1 name lookups.
"""

import sys
from pathlib import Path

GAME = Path(r"H:\GOG\BloodRayne 2 Terminal Cut")
sys.path.insert(0, str(GAME / "MODDING" / "tools"))
from pod3 import Pod3  # noqa: E402

RENAMES = (
    (b"collision\x00",  b"norm_collision\x00\x00"),   # state 0
    (b"collision1\x00", b"rage_collision1\x00"),      # state 1
    (b"collision2\x00", b"fury_collision2\x00"),      # state 2
)
FIELD = 16


def main() -> None:
    models = Pod3(GAME / "W32MODEL.POD")
    data = bytearray(models.read(models.by_name["MODELS\\WEAPON_DHAMPIR_BLADES.SMB"]))
    for old, new in RENAMES:
        assert len(new) <= FIELD
        pos = data.find(old)
        assert pos >= 0, f"{old!r} not found"
        assert data.find(old, pos + 1) < 0, f"{old!r} not unique"
        data[pos:pos + FIELD] = new.ljust(FIELD, b"\x00")
        print(f"  @{pos:06X}: {old!r} -> {new.rstrip()!r}")
    out = GAME / "models" / "br2_dhampir_blades.smb"
    out.write_bytes(data)
    print(f"Wrote {out} ({len(data)} bytes)")


if __name__ == "__main__":
    main()
