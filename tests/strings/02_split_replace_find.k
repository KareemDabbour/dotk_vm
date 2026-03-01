var s = "a,b,c,b"
var parts = s.split(",")
print(len(parts))
print(parts[1])
print(s.find("b"))
print(s.replace("b", "X"))

# EXPECTED
# 4
# b
# 2
# a,X,c,X
# END_EXPECTED
