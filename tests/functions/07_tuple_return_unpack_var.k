fn pair(x, y) {
  return (x, y)
}

var (a, b) = pair(7, 9)
print(a)
print(b)

var (head, *middle) = pair("L", "R")
print(head)
print(middle)

# EXPECTED
# 7
# 9
# L
# [R]
# END_EXPECTED
