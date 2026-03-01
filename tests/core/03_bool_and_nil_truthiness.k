if (true) {
  print(1)
} else {
  print(0)
}

if (false) {
  print(0)
} else {
  print(2)
}

var n = null
if (n) {
  print(0)
} else {
  print(3)
}

# EXPECTED
# 1
# 2
# 3
# END_EXPECTED
