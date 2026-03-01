var xs = {1, 2, 3, 4}
var ys = xs.map(fn(n) => n * 2)
print(len(ys))
print(ys[0])
print(ys[3])

var sum = 0
for (var i = 0; i < len(ys); i += 1) {
	sum = sum + ys[i]
}
print(sum)

# EXPECTED
# 4
# 2
# 8
# 20
# END_EXPECTED
