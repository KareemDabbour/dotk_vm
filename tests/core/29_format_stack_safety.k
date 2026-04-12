# Ensure format/f-string interpolation does not corrupt stack when output grows large.

var xs = {}
var i = 0
while(i < 3000){
    xs.append(i)
    i += 1
}

var out = "list=${0}".format(xs)
if (type(out) != "string"){
    print("FAIL")
    exit(1)
}
if (len(out) < 10000){
    print("FAIL")
    exit(1)
}

print("ok")

# EXPECTED
# ok
# END_EXPECTED
