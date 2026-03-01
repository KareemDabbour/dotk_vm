var m = .{"a"::1, "b"::2}
print(len(m))
print(m["a"])
m["c"] = 3
print(len(m))
print(m["c"])

# EXPECTED
# 2
# 1
# 3
# 3
# END_EXPECTED
