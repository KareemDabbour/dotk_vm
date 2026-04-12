import "socket.k"

fn probe(){
    var s = Socket.withDefaults()
    var ev = {"JOIN", s, "user", ""}
    var r = {true, 0, ev, false, false}

    var ev2 = r[2]
    var kind = ev2[0]
    var sock = ev2[1]
    var name = ev2[2]
    var payload = ev2[3]

    if (kind != "JOIN"){
        print("FAIL")
        exit(1)
    }
    if (sock != s){
        print("FAIL")
        exit(1)
    }
    if (name != "user" || payload != ""){
        print("FAIL")
        exit(1)
    }
}

probe()
print("ok")

# EXPECTED
# ok
# END_EXPECTED
