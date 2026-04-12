fn ret42(x) {
  return 42
}

fn side() {
  print("side")
  return 9
}

print(ret42(7))
print(ret42(side()))

# EXPECTED
# 42
# side
# 42
# END_EXPECTED
