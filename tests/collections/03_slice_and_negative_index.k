var xs = {10, 20, 30, 40, 50}
print(xs[-1])
print(",".join(xs[1:4]))
print(",".join(xs[:3]))
print(",".join(xs[2:]))
print(",".join(xs[:@-1]))

# EXPECTED
# 50
# 20,30,40
# 10,20,30
# 30,40,50
# 50,40,30,20,10
# END_EXPECTED
