var before = vmStats().bytesAllocated

fn buildGarbage() {
  var xs = {}
  for (var i = 0; i < 1500; i += 1) {
    xs.append("item-" + str(i))
  }
  return len(xs)
}

print(buildGarbage() > 1000)
var freed = gc()
var after = vmStats().bytesAllocated

print(type(freed))
print(freed >= 0)
print(type(after))
print(type(before))

fn discardStrings(batch) {
  for (var i = 0; i < 1500; i += 1) {
    str(batch * 1500 + i)
  }
}
discardStrings(0)
gc()
var warm = vmStats().bytesAllocated
for (var batch = 1; batch < 12; batch += 1) {
  discardStrings(batch)
  gc()
}
print(vmStats().bytesAllocated < warm + 131072)
var keep = str(987654321)
gc()
print(keep)

# EXPECTED
# true
# int
# true
# int
# int
# true
# 987654321
# END_EXPECTED
