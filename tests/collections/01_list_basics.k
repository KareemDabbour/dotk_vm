var xs = {1, 2, 3}
xs.append(4)
print(len(xs))
print(xs[0])
print(xs[3])
print(xs.pop())
print(len(xs))

var combined = {1} + {2, 3}
combined.append(4)
print(",".join(combined))
print(",".join({} + {5, 6}))
print(",".join({7, 8} + {}))
var empty = {} + {}
empty.append(9)
print(empty[0])

# EXPECTED
# 4
# 1
# 4
# 4
# 3
# 1,2,3,4
# 5,6
# 7,8
# 9
# END_EXPECTED
