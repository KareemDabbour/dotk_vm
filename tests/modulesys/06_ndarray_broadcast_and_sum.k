from ndarray import array, add, sum, sumAxis, toList;

var a = array({{1, 2, 3}, {4, 5, 6}})
var b = array({{10, 20, 30}})

print(toList(add(a, b)))
print(toList(add(a, 1)))
print(toList(add(2, a)))
print(sum(a))
print(toList(sumAxis(a, 0)))
print(toList(sumAxis(a, 1)))

# EXPECTED
# [[11,22,33],[14,25,36]]
# [[2,3,4],[5,6,7]]
# [[3,4,5],[6,7,8]]
# 21
# [5,7,9]
# [6,15]
# END_EXPECTED
