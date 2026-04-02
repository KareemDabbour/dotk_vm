import "socket.k"

var PORT = 8082
var CHAT_FILE = "chat_1.log"

fn ensureChatFile(){
    with(File(CHAT_FILE, "a+"), fn(f) {
        # create file if missing
    })
}

fn appendLine(line){
    with(File(CHAT_FILE, "a+"), fn(f) {
        f.write(line + "\n")
    })
}

fn readAllChat(){
    var out = ""
    with(File(CHAT_FILE, "r"), fn(f) {
        out = f.read()
    })
    return out
}

fn processMessage(raw){
    if (!raw || raw.isWhitespace()){
        return readAllChat()
    }

    if (raw == "GET"){
        return readAllChat()
    }

    var parts = raw.split("|")
    if (len(parts) == 0){
        return readAllChat()
    }

    if (parts[0] == "CON" && len(parts) >= 2){
        appendLine("CON|" + parts[1])
        return readAllChat()
    }

    if (parts[0] == "MSG" && len(parts) >= 3){
        appendLine("MSG|" + parts[1] + "|" + parts[2])
        return readAllChat()
    }

    return readAllChat()
}

fn handleClientFd(fd){
    var client = Socket.fromFd(fd)
    try{
        while(!client.isClosed){
            var raw = client.receive()
            if (raw == ""){
                client.close()
                break
            }
            var response = processMessage(raw)
            client.send(response)
        }
    }catch(e){
        print(f"client handler error: {e}")
        try { client.close() } catch(_) {}
    }
    return 0
}

ensureChatFile()

var server = Socket.withDefaults()
server.bindToPort(PORT)
server.startListening()
print(f"chatserver_1 listening on localhost:{PORT}")

while(true){
    var client = server.accept()
    var clientFd = client.getSocketFd()

    var pid = spawn(fn(fd){
        return handleClientFd(fd)
    }, clientFd)

    # parent does not need this accepted socket fd copy
    client.close()

    # optional: reap a finished child without blocking
    waitPid(pid, true)
}
