fn f(a, b, c) {
  print(a)
  print(b)
  print(c)
}

f(10, @c=30, @b=20)

# EXPECTED
# 10
# 20
# 30
# END_EXPECTED
