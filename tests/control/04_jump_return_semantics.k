fn f(x) {
  if (x) {
    return 10
  }
  return 20
}

print(f(true))
print(f(false))

# EXPECTED
# 10
# 20
# END_EXPECTED
