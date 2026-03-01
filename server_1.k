import "socket.k"
import "file"

var sock = Socket.withDefaults()
sock.bindToPort(8080)
sock.startListening()

fn handlePost(r){
    var pathStr = r.getPathStr()
    if (!pathStr || pathStr == "/"){
        return false
    }

    var rel = pathStr[1:]
    if (!rel || contains(rel, "..")){
        return false
    }

    with(File(rel, "w+"), fn(f) {
        f.write(r.body.toStr())
    })

    return true
}

fn handleGet(r){
    var ret
    var pathStr = r.getPathStr()
    if(!pathStr)
        return ret

    if (contains(pathStr, "..")){
        return ret
    }

    var path = StringBuilder(".")
    path.append(pathStr)

    if (pathStr[-1] == "/"){
        path.append("index.html")
    }else if (!contains(pathStr, ".")){
        path.append("/").append("index.html")
    }

    ret = path.toStr()
    return ret
}

fn processRequest(reqRaw){
    
    var request = Request.parse(reqRaw)

    switch(request.getType()){
        case RequestType.GET {
            return handleGet(request)
        }
        case RequestType.POST {
            if (handlePost(request)){
                return "__POST_OK__"
            }
            return null
        }
        default {
            return null
        }
    }
}

fn send404(sockIn){
    sockIn.send("HTTP/1.1 404 NOT FOUND\r\nContent-Type: text/html\r\n\r\n<html><body><h1>404 NOT FOUND</h1></body></html>")
}

fn send500(sockIn){
    sockIn.send("HTTP/1.1 500 Internal Server Error\r\nContent-Type: text/html\r\n\r\n<html><body><h1>500 Internal Server Error</h1></body></html>")
}

fn handleClientFd(fd){
    var sockIn = Socket.fromFd(fd)
    try{
        var request = sockIn.receive()
        var response = processRequest(request)

        if (!response){
            send404(sockIn)
        } else if (response == "__POST_OK__"){
            sockIn.send("HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n\r\nok")
        } else if (!sockIn.sendFile(response)){
            send404(sockIn)
        }
    }catch(e){
        send500(sockIn)
    }

    try { sockIn.close() } catch (_) {}
    return 0
}

print("server_1 listening on localhost:8080")

while(true){
    var accepted = Socket()
    try {
        accepted = sock.accept()
        var fd = accepted.getSocketFd()

        spawn(fn(clientFd){
            return handleClientFd(clientFd)
        }, fd)

        accepted.close()
    } catch(e) {
        try { accepted.close() } catch (_) {}
    }
}
