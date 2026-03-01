fn g(a, b, *rest) {
  print(a)
  print(b)
  print(len(rest))
}

g(100, 200, 1, 2, 3, @b=20, @a=10)

# EXPECTED
# 10
# 20
# 5
# END_EXPECTED
