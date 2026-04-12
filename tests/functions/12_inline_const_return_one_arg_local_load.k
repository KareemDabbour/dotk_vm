fn callWithLocal() {
  var y = 9
  return (fn(x) {
    return 42
  })(y)
}

print(callWithLocal())

# EXPECTED
# 42
# END_EXPECTED
