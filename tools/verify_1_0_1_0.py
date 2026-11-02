"""Verify the updated Find hooks against a local 1.0.1.0 executable (read-only)."""
import pathlib,re,struct,sys
raw=pathlib.Path(sys.argv[1]).read_bytes();pe=struct.unpack_from("<I",raw,60)[0]
assert struct.unpack_from("<I",raw,pe+8)[0]==0x6ae4601e,"Expected client 1.0.1.0"
root=pathlib.Path(__file__).resolve().parent.parent
checked=0
for source in (root/"src").glob("*.cpp"):
 for pattern in re.findall(r'FindUnique\("([0-9A-F? ]+)"',source.read_text(encoding="utf-8")):
  rx=b"".join(b"." if "?" in x else re.escape(bytes.fromhex(x)) for x in pattern.split())
  hits=[m.start() for m in re.finditer(rx,raw,re.S)]
  assert len(hits)==1,(source.name,pattern,hits);checked+=1
print("PASS:",checked,"literal Find hooks uniquely match client 1.0.1.0")
print("Native fixed UI mesh bindings live in the external common/ui_mesh.h; use the matching 1.0.1.0 common header.")
