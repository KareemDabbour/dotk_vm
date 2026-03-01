fn sum(a, b, *rest) {
  var s = a + b;
  var i = 0;
  while (i < len(rest)) {
    s = s + rest[i];
    i = i + 1;
  }
  return s;
}

print(sum(1, 2, 3, 4))
print(sum(6, 7, @a=1, @b=5))

# EXPECTED
# 10
# 19
# END_EXPECTED
