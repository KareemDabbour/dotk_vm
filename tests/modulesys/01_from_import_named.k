from modules.mathx import abs, PI;

print(abs(-5));
print(PI);

var constBlocked = false;
try {
	PI = 4;
} catch (e) {
	constBlocked = true;
}

print(constBlocked);

# EXPECTED
# 5
# 3.14
# true
# END_EXPECTED
