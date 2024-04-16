import "socket.k";
import "file.k";

var sock = Socket.withDefaults();
sock.bindToPort(8080);
sock.startListening();

fn handlePost(r){
    print(with(File(r.getPathStr()[1:], F_RW, null), fn(f) => f.write(r.body.toStr())))
}

fn handleGet(r){
    var ret;
    var pathStr = r.getPathStr();
    if(!pathStr)
        return ret;
    var path = StringBuilder(".");

    var queryParams;
    path.append(pathStr);
    if (pathStr[-1] == "/"){
        path.append("index.html");
    }else if (!contains(pathStr, ".")){
        path.append("/").append("index.html");
    }
    ret = path.toStr();
    return ret;
}

fn processRequest(req){

    var request = Request.parse(req);
    print(request);
    switch(request.getType()){
        case RequestType.GET 
            return handleGet(request);
        case RequestType.POST {
            return handlePost(request);
        }
        default  {
            print("UNKNOWN Request: " + request.toStr());
        }
    }
    
}

fn handle(sockIn){

    try{
        var request = sockIn.receive();
        var response = processRequest(request);
        if (!response){
            sockIn.send("HTTP/1.1 404 NOT FOUND\r\nContent-Type: text/html\r\n\r\n<html><body><h1>404 NOT FOUND</h1></body></html>");
        }else if(!sockIn.sendFile(response)){
            sockIn.send("HTTP/1.1 404 NOT FOUND\r\nContent-Type: text/html\r\n\r\n<html><body><h1>404 NOT FOUND</h1></body></html>");
        }

    }catch(e){
        print ("ERROR in HANDLE::${}".f(e));
        sockIn.send("HTTP/1.1 404 NOT FOUND\r\nContent-Type: text/html\r\n\r\n<html><body><h1>404 NOT FOUND</h1></body></html>");
        
    }

}

while(true){
    try {
        var new_sock = sock.accept();
        handle(new_sock);
        new_sock.close();
    } catch(e) {
        print("ERROR::${}".f(e));
    }
}


sock.close();