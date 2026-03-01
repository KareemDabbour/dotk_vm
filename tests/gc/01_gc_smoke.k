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

# EXPECTED
# true
# int
# true
# int
# int
# END_EXPECTED
