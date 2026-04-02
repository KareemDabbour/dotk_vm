from socket import Socket

var PORT = 62201

# Each entry: [socket, name]
var clients = []

fn broadcast(msg){
    var i = 0
    while(i < len(clients)){
        try {
            clients[i][0].send(msg)
            i += 1
        } catch(e){
            print(f"  broadcast error to {clients[i][1]}: {e}")
            try { clients[i][0].close() } catch(_) {}
            clients.pop(i)
        }
    }
}

fn removeClient(sock){
    var i = 0
    while(i < len(clients)){
        if (clients[i][0] == sock){
            clients.pop(i)
            return;
        }
        i += 1
    }
}

# ── Per-client handler (runs in its own thread) ──
async fn handleClient(client){
    var name = "?"
    try {
        # First message must be CON|username
        var raw = client.receive()
        if (raw == ""){
            client.close()
            return 0
        }
        var parts = raw.split("|")
        if (len(parts) >= 2 && parts[0] == "CON")
            name = parts[1]
        else
            name = "anon"

        clients.append({client, name})
        print(f"[+] {name} joined  ({len(clients)} online)")
        broadcast(f"CON|{name}\n")

        # Read loop – block until client sends something
        while(true){
            var raw = client.receive()
            if (raw == "") break

            var parts = raw.split("|")
            if (parts[0] == "MSG" && len(parts) >= 3){
                var line = f"MSG|{parts[1]}|{parts[2]}\n"
                print(f"  {name}: {parts[2]}")
                broadcast(line)
            }
        }
    } catch(e){
        print(f"  handler error ({name}): {e}")
    }

    # Clean up
    removeClient(client)
    try { client.close() } catch(_) {}
    print(f"[-] {name} left  ({len(clients)} online)")
    broadcast(f"SYS|{name} left the chat\n")
    return 0
}

# ── Reap finished worker threads ──
fn reapFinishedWorkers(workers){
    var i = 0
    while(i < len(workers)){
        if (!workers[i].isAlive()){
            try { workers[i].join() } catch(_) {}
            workers.pop(i)
        } else {
            i += 1
        }
    }
}

# ── Main ──
var server = Socket.withDefaults()
server.bindToPort(PORT)
server.startListening()
print(f"Push chat server listening on localhost:{PORT}")

var workers = []

while(true){
    reapFinishedWorkers(workers)
    var client = server.accept()
    var worker = handleClient(client)
    workers.append(worker)
}
