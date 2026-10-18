var xs = {10, 20, 30, 40, 50}
print(xs[-1])
print(",".join(xs[1:4]))
print(",".join(xs[:3]))
print(",".join(xs[2:]))

print(",".join(xs[-100:2]))
print(len(xs[-100:-100]))
print(",".join(xs[:@-1]))
print(",".join(xs[1:4@-2]))
print(",".join(xs[0:5@2147483647]))
print(",".join(xs[0:5@-2147483648]))
print("abcde"[-100:2])
print("abcde"[:@-1])
print("abcde"[0:5@2147483647])
try {
	print(xs[0:2@0])
} catch (error) {
	print(error.message)
}
try {
	print("abc"[0:2@0])
} catch (error) {
	print(error.message)
}
try {
	print(xs[0:1@2147483648])
} catch (error) {
	print(error.message)
}
print("continued")

# EXPECTED
# 50
# 20,30,40
# 10,20,30
# 30,40,50
# 10,20
# 0
# 50,40,30,20,10
# 40,20
# 10
# 50
# ab
# edcba
# a
# Slice step cannot be zero
# Slice step cannot be zero
# Slice components must be finite numbers in integer range
# continued
# END_EXPECTED
