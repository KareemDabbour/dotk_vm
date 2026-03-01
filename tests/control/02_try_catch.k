var caught = false

try {
  var xs = {1}
  print(xs[5])
} catch (e) {
  caught = true
  print(type(e))
  print(type(e.message))
}

print(caught)

# EXPECTED
# object
# string
# true
# END_EXPECTED
