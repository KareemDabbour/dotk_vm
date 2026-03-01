var m = .{}
m["k"] = {1, 2, 3, 4}
print(len(m["k"]))
print(gc() >= 0)
print(len(m["k"]))

# EXPECTED
# 4
# true
# 4
# END_EXPECTED
