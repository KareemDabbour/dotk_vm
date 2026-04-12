print((fn() { return 42 })())
print((fn() { return true })())
print((fn() { return "ok" })())

# EXPECTED
# 42
# true
# ok
# END_EXPECTED
