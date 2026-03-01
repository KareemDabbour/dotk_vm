fn kw(a, b, *rest) {
  print(a)
  print(b)
  print(len(rest))
}

kw(9, @a=1, @b=2)

# EXPECTED
# 1
# 2
# 1
# END_EXPECTED
