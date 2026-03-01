var s = "  AbC  "
print(s.trim())
print("|" + s.lower() + "|")
print("|" + s.upper() + "|")
print("abc".contains("b"))

# EXPECTED
# AbC
# |  abc  |
# |  ABC  |
# true
# END_EXPECTED
