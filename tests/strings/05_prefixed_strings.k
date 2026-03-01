import "file"

var a = b"ABC"
var h = x"41 42 43"
var n = bin"01000001 01000010 01000011"

print(a)
print(h)
print(n)
print(a == h && h == n)

var p = "temp/prefixed_modes.bin"
var f = File(p, "wb+")
f.write(n)
f.resetCursor()
print(f.read(3))
f.close()

# EXPECTED
# ABC
# ABC
# ABC
# true
# ABC
# END_EXPECTED
