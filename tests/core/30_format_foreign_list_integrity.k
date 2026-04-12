import "socket.k"

var s = Socket.withDefaults()
var ev = {"MSG", s, "kareem", "hello"}
var r = {true, 0, ev, false, false}

var dump = "r=${0}".format(r)
if (type(dump) != "string"){
    print("FAIL")
    exit(1)
}

if (type(r[2]) != "list"){
    print("FAIL")
    exit(1)
}
if (r[2][0] != "MSG"){
    print("FAIL")
    exit(1)
}
if (r[2][1] != s){
    print("FAIL")
    exit(1)
}
if (r[2][2] != "kareem" || r[2][3] != "hello"){
    print("FAIL")
    exit(1)
}

print("ok")

# EXPECTED
# ok
# END_EXPECTED
