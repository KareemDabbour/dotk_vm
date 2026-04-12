var t = 0

fn selfJoin() {
  return t.join()
}

t = Thread(selfJoin)

var caught = false
try {
  print(t.join())
} catch (e) {
  caught = true
  print(type(e))
  print(type(e.message))
}

print(caught)
print(t.isAlive())

# EXPECTED
# object
# string
# true
# false
# END_EXPECTED