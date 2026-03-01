var i = 1
var sum1 = 0
while (i <= 5) {
  sum1 = sum1 + i
  i = i + 1
}
print(sum1)

var sum2 = 0
for (var j = 0; j < 4; j += 1) {
  sum2 = sum2 + j
}
print(sum2)

# EXPECTED
# 15
# 62
# END_EXPECTED
