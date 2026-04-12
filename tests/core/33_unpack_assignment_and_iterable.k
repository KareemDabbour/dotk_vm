var a = 0
var b = 0
;
(a, b) = (7, 9)
print(a)
print(b)

var first = ""
var rest = []
;
(first, *rest) = "hey"
print(first)
print(rest)

fn pairGen() {
    yield 4
    yield 5
}

var p = 0
var q = 0
;
(p, q) = pairGen()
print(p + q)

# EXPECTED
# 7
# 9
# h
# [e,y]
# 9
# END_EXPECTED
