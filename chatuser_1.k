import "socket.k"
import "lang.k"

var port = 8082
var host = "localhost"
var username = f"user{randInt(0, 1000)}"

if (ARG_V.indexOf("-p") != -1){
    port = ARG_V[ARG_V.indexOf("-p") + 1]
}
if (ARG_V.indexOf("-u") != -1){
    username = ARG_V[ARG_V.indexOf("-u") + 1]
}

var chat = []

fn render_chat(raw){
    if (!raw){
        return;
    }

    var lines = raw.split("\n")
    if (len(lines) == len(chat)){
        return;
    }

    clear()
    var parsed = []

    for(var i = 0; i < len(lines); i += 1){
        var line = lines[i]
        if (!line || line.isWhitespace()) continue

        parsed.append(line)
        var parts = line.split("|")
        if (len(parts) < 2) continue

        if (parts[0] == "CON"){
            var u = parts[1]
            print f"{Ansi.ALL_COLORS[hash(u) % len(Ansi.ALL_COLORS)]}[{u}]{Ansi.RESET} CONNECTED"
        } else {
            if (parts[0] == "MSG" && len(parts) >= 3){
                var u = parts[1]
                var msg = parts[2]
                print "${}[${}]${} ${}".f(Ansi.ALL_COLORS[hash(u) % len(Ansi.ALL_COLORS)], u, Ansi.RESET, msg)
            }
        }
    }

    chat = parsed
}

var conn = Socket.newSocket()
conn.port = port
conn.host = host

with(conn, fn(in) {
    if (!in.connect()){
        print "Failed to connect to the server"
        return;
    }

    print f"Connected as {username} to {host}:{port}"
    print "Type /quit to exit"

    in.send("CON|" + username)
    render_chat(in.receive())

    try {
        while(true){
            if (kbhit()){
                var data = input()
                if (data == "/quit"){
                    in.close()
                    break
                }
                in.send("MSG|" + username + "|" + data)
            }else{
                in.send("GET")
            }

            render_chat(in.receive())
            sleep(0.5)
        }
    } catch (e){
        print("Disconnected from server")
    }
})
