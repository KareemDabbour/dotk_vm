# Test native functions with kwargs and default values

# round() now accepts kwargs: round(value, digits=0)
print(round(3.14159))
print(round(3.14159, 2))
print(round(3.14159, @digits=3))
print(round(@digits=1, @value=2.75))

# EXPECTED
# 3
# 3.14
# 3.142
# 2.8
# END_EXPECTED
