import "file"

var p = "temp/test_chunk_read_builtin.txt"
var f = File(p, "w+")
f.write("abcdef")
f.resetCursor()

print(f.read(2))
print(f.read(2))
print(f.read(10))
print(len(f.read()))

f.close()

# EXPECTED
# ab
# cd
# ef
# 0
# END_EXPECTED
