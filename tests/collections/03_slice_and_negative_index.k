var xs = {10, 20, 30, 40, 50}
print(xs[-1])
print(",".join(xs[1:4]))
print(",".join(xs[:3]))
print(",".join(xs[2:]))

# EXPECTED
# 50
# 20,30,40
# 10,20,30
# 30,40,50
# END_EXPECTED
