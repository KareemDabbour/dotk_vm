var m = .{"a"::1}
var caught = false

try {
  print(m.a)
} catch (e) {
  caught = true
}

import modules.mathx as mathx
print(caught)
print(mathx.lcm(2, 3))

# EXPECTED
# true
# 6
# END_EXPECTED
