a = 1
a = a + 2
print(a)

{
  b = 5
  b += 2
}
print(b) # This works because b is implicitly global

const c = 9
print(c)

try {
  c = 10
} catch (e) {
    print(e.message)
}

# EXPECTED
# 3
# 7
# 9
# Cannot assign to constant 'c'.
# END_EXPECTED
