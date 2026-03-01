var xs = {}
for (var i = 0; i < 100000; i += 1) {
  xs.append("!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!")
}

var s = str(xs)
print(type(s))
print(len(s) > 0)
print(len(s) > 200000)

# EXPECTED
# string
# true
# true
# END_EXPECTED
