from "ndarray" import array, ones, matmul, transpose, toList;

var a = array({{1, 2}, {3, 4}})
var b = ones({2, 2})

print(toList(matmul(a, b)))
print(toList(transpose(a)))

# EXPECTED
# [[3,3],[7,7]]
# [[1,3],[2,4]]
# END_EXPECTED
