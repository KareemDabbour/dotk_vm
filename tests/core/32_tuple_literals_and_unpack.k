print((1 + 2))
print((1 + 2,))
print((1, 2, 3))

var (x, y) = (10, 20)
print(x + y)

var (a, *rest) = (1, 2, 3, 4, 5)
print(a)
print(rest)

var (c, d) = "ok"
print(c)
print(d)

# EXPECTED
# 3
# (3,)
# (1, 2, 3)
# 30
# 1
# [2,3,4,5]
# o
# k
# END_EXPECTED
