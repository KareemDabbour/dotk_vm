async fn burn(n) {
  var x = 0
  for (var i = 0; i < n; i = i + 1) {
    x = x + i
  }
  return x
}

var keep = burn(30000)

for (var i = 0; i < 24; i = i + 1) {
  burn(30000)
}

var freed = gc()

print(await keep > 0)
print(type(freed))
print(freed >= 0)

# EXPECTED
# true
# int
# true
# END_EXPECTED