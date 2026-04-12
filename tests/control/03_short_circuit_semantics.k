fn side() {
  print("side")
  return 99
}

print(true || side())
print(false || 7)
print(false && side())
print(true && 8)

# EXPECTED
# true
# 7
# false
# 8
# END_EXPECTED
