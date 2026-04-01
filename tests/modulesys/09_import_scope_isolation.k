import modules.textx as textx

var leakedGreet = false
try {
  greet("bob")
} catch (e) {
  leakedGreet = true
}

print(leakedGreet)
print(textx.greet("bob"))

from modules.mathx import abs

var leakedPi = false
try {
  PI
} catch (e) {
  leakedPi = true
}

print(leakedPi)
print(abs(-3))

# EXPECTED
# true
# hi bob
# true
# 3
# END_EXPECTED
