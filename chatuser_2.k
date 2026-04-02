import "socket.k"
import "lang.k"

var port = 62201
var host = "localhost"
var username = f"user{randInt(0, 1000)}"

if (ARG_V.indexOf("-p") != -1){
    port = int(ARG_V[ARG_V.indexOf("-p") + 1])
}
if (ARG_V.indexOf("-u") != -1){
    username = ARG_V[ARG_V.indexOf("-u") + 1]
}
if (ARG_V.indexOf("-h") != -1){
    host = ARG_V[ARG_V.indexOf("-h") + 1]
}

var running = true

fn colorFor(u){
    return Ansi.ALL_COLORS[hash(u) % len(Ansi.ALL_COLORS)]
}

fn renderLine(line){
    if (!line || line.isWhitespace()) return;
    var parts = line.split("|")
    if (len(parts) < 2) return;

    if (parts[0] == "CON"){
        var u = parts[1]
        print f"{colorFor(u)}[{u == username ? "me" : u}]{Ansi.RESET} joined"
    }
    if (parts[0] == "MSG" && len(parts) >= 3){
        var u = parts[1] 
        var msg = parts[2]
        print f"{colorFor(u)}[{u == username ? "me" : u}]{Ansi.RESET} {msg}"
    }
    if (parts[0] == "SYS"){
        print f"{Ansi.YELLOW}* {parts[1]}{Ansi.RESET}"
    }
}

# ── Reader thread: blocks on receive(), renders each push immediately ──
async fn reader(conn){
    # if (conn == null || conn.isClosed()){
    #     print("No connection object for reader thread")
    #     return 0
    # }
    print(conn)
    while(running){
        print("Reader thread waiting for data...")
        try {
            var data = conn.receive()
            if (data == ""){
                if (!running) return 0
                print(f"{Ansi.RED}Server closed connection{Ansi.RESET}")
                running = false
                return 0
            }
            # Server may batch multiple lines in one send
            var lines = data.split("\n")
            for (var i = 0; i < len(lines); i += 1)
                renderLine(lines[i])
        } catch(e){
            if (!running) return 0
            print(f"{Ansi.RED}Connection lost: {e}{Ansi.RESET}")
            running = false
            return 0
        }
    }
    print("Reader thread exiting")
    return 0
}

# ── Main ──
var conn = Socket.newSocket()
conn.port = port
conn.host = host
if (!conn.connect()){
    print "Failed to connect to server"
    running = false
}

if (running){
    # Register with server
    conn.send("CON|" + username)

    print f"Connected as {colorFor(username)}[{username}]{Ansi.RESET} to {host}:{port}"
    print "Type /quit to exit"
    print "---"

    # Start reader thread (blocking receive, no polling)
    var readerThread = reader(conn)

    try {
        while(running){
            if (kbhit()){
                var data = input()
                if (data == "/quit"){
                    running = false
                    break
                }
                if (data != "")
                    conn.send("MSG|" + username + "|" + data)
            }
            sleep(0.05)
        }
        print "Disconnecting..."
    } catch(e){
        print("Disconnected")
    }

    running = false
    try { conn.close() } catch(_) {}
    print "closing reader thread..."
    readerThread.join() 
    print "Goodbye!"
    print readerThread.isAlive()
    print readerThread.result()
    print(readerThread)

}
