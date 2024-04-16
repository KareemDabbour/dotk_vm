import "stdlib.k";
import "json.k";

class Socket{
    SOL_SOCKET = 1;
    SO_ACCEPTCONN = 30;
    SO_BROADCAST = 6;
    SO_DONTROUTE = 5;
    SO_ERROR = 4;
    SO_KEEPALIVE = 9;
    SO_LINGER = 13;
    SO_OOBINLINE = 10;
    SO_RCVBUF = 8;
    SO_RCVLOWAT = 18;
    SO_RCVTIMEO = 20;
    SO_REUSEADDR = 2;
    SO_SNDBUF = 7;
    SO_SNDLOWAT = 19;
    SO_SNDTIMEO = 21;
    SO_TYPE = 3
    LOOPBACK = "localhost"
    init(){
        this.socket = null;
        this.port = null;
        this.host = null;
    }
    bindToPort(port){
        this.port = port;
        return bind(this.socket, this.port);
    }
    setHost(host){
        this.host = host;
        return this;
    }
    setDefaultSocketOptions(){
        setSockOpt(this.socket, Socket.SOL_SOCKET, Socket.SO_REUSEADDR);
    }
    setSocketOptions(level, option){
        if(type(level) != "int" || type(option) != "int"){
            print_error("level and option must be int");
        }
        setSockOpt(this.socket, level, option);
    }
    startListening(){
        listen(this.socket);
    }
    accept(){
        if(!this.port)
            print_error("No bound port");
        var new_sock = Socket.fromFd(accept(this.socket, this.port));
        if(!new_sock)
            print_error("accept failed");
        return new_sock;
    }
    connect(){
        if(!this.host)
            print_error("No host");
        if(!this.port)
            print_error("No port");
        return connect(this.socket, this.host, this.port);
    }
    close(){
        close(this.socket);
    }
    send(strData){
        return send(this.socket, strData);
    }
    sendRequest(req){
        if(!instanceof(req, Request)){
            print_error("req must be Request");
        }
        return this.sendFile(req.getPathStr());
    }
    receive(){
        return read(this.socket);
    }
    getSocketFd(){
        return this.socket;
    }
    getBoundPort(){
        return this.port;
    }
    sendFile(path){
        if (type(path) != "string"){
            print_error("path must be string");
        }
        return sendFile(this.socket, path);
    }
    fromFd(fd){
        if (type(fd) != "int"){
            print_error("fd must be int");
        }
        if(fd < 0){   
            print_error("fd must be positive");
        }
        var ret = Socket();
        ret.socket = fd;
        return ret;
    }
    newSocket(){
        var ret = Socket();
        ret.socket = socket();
        if(ret.socket < 0){
            print_error("socket creation failed");
            return null;
        }
        return ret;
    }
    withDefaults(){
        var ret = Socket();
        ret.socket = socket();
        ret.setHost(Socket.LOOPBACK);
        if(ret.socket < 0){
            print_error("socket creation failed");
            return null;
        }
        ret.setDefaultSocketOptions();
        return ret;
    }
}

class RequestType{
    init(val, strVal){
        this.val = val;
        this.strVal = strVal;
    }
    UNKNOWN = RequestType(0, "UNKNOWN");
    POST = RequestType(1, "POST");
    GET = RequestType(2, "GET");
    allValidValues = {
        RequestType.UNKNOWN,
        RequestType.POST,
        RequestType.GET
    };
    toStr(){
        return this.strVal;
    }
    _eq_(other){  
        switch(type(other)){
            case "int" {
                return this.val == other;
            }
            case "string" {
                return this.strVal == other;
            }   
            case "object" {
                if(!instanceof(other, RequestType))
                    return false;
                return this.val == other.val && this.strVal == other.strVal;
            }
            default {
                return false;
            }
        }
    }
    valueOf(reqType){
        if(type(reqType) == "int"){
            return RequestType.allValidValues[i];
        }
        else if(type(reqType) == "string"){
            for(var i = 0; i < len(RequestType.allValidValues); i+=1){
                if(RequestType.allValidValues[i].strVal == reqType){
                    return RequestType.allValidValues[i];
                }
            }
        }
        else if(instanceof(reqType, RequestType)){
            return reqType;
        }
        return RequestType.UNKNOWN;
        
    }
}

class Request{
    init(){
        this.type = RequestType.UNKNOWN;
        this.path = null;
        this.body = HashMap();
    }
    getType(){
        return this.type;
    }
    setType(type){
        this.type = RequestType.valueOf(type);
        return this;
    }
    getPath(){
        return this.path;
    }
    getPathStr(){
        if(this.path)
            return this.path.getPath();
        else
            return "";
    }
    setPath(path){
        this.path = path;
        return this;
    }
    getBody(){
        return this.body;
    }
    setBody(body){
        if (!body && !instanceof(body, HashMap)){
            print_error("body must be a Map");
            return null;
        }
        this.body = body;
        return this;
    }
    parse(reqString){
        var delim = "\n";
        if(reqString.contains("\r")){
            delim = "\r\n";
        }
        var lines = reqString.split(delim);
        var ret = Request();
        if(len(lines)){

            var firstLine = lines[0].split(" ");
            var firstLineLen = len(firstLine);
            if(firstLineLen > 0) ret.setType(RequestType.valueOf(firstLine[0]));
            if(firstLineLen > 1) ret.setPath(Path.parse(firstLine[1]));
            ret.parseBody(lines[-1]);
        }
        return ret;
    }
    toStr(){
        var sb = StringBuilder("Request: { type: ");
        sb.append(this.type.toStr());
        if(this.path){
            sb.append(" ")
            .append(this.path.toStr());
            # .append(", ");
        }
        if(len(this.body)){
            sb.append(", Body: ")
            .append(this.body.toStr());
        }
        sb.append(" }");
        return sb.toStr();
    }
    toHttpString(host){
        var sb = StringBuilder(this.type.toStr());
        sb
        .append(" ")
        .append(this.path.getPath())
        .append(" HTTP/1.1\r\n")
        .append("Accept: text/html,application/xhtml+xml,application/xml;q=0.9,image/avif,image/webp,image/apng,*/*;q=0.8,application/signed-exchange;v=b3;q=0.7\r\n")

        .append("Accept-Encoding: gzip, deflate")
        .append("\r\n")
        .append("Accept-Language: en-US,en;q=0.9,fr-CA;q=0.8,fr;q=0.7")
        .append("\r\n")
        .append("Connection: keep-alive")
        .append("\r\n")
        .append("DNT: 1")
        .append("\r\n")
        .append("Host: ")
        .append(host)
        .append("\r\n")
        .append("\r\n");
        # if(!this.body.isEmpty()){
        #     sb.append("Content-Length: ")
        #     .append(len(this.body.toStr()))
        #     .append("\r\n\r\n")
        #     .append(this.body.toStr());
        # }
        # else{
        #     sb.append("\r\n");
        # }
        return sb.toStr();
    }
    parseBody(body){
        if (type(body) != "string"){
            print_error("body must be string");
            return this;
        }
        if(Strings.isWhitespace(body)){
            return this;
        }
        var params = split(body, "&");
        for(var i = 0; i < len(params); i+=1){
            var param = split(params[i], "=");
            var val;
            if(len(param) > 1){
                val = param[1];
            }
            else{
                val = "";
            }
            this.body[param[0]]= val;
        }
        return this;
    }
}

class Response{
    init(){
        this.statusCode = 0;
        this.body = Map();
    }

    getStatusCode(){
        return this.statusCode;
    }
    setStatusCode(statusCode){
        if(type(statusCode) != "int"){
            print_error("statusCode must be int");
            return null;
        }
        this.statusCode = statusCode;
        return this;
    }
    getBody(){
        return this.body;
    }
    setBody(body){
        if (!body && !instanceof(body, Map)){
            print_error("body must be a Map");
            return null;
        }
        this.body = body;
        return this;
    }
    parse(resStr){
        if(type(resStr) != "string"){
            print_error("resStr must be string");
            return null;
        }
        resStr = trim(resStr);
        var delim = "\n";
        if(contains(resStr, "\r")){
            delim = "\r\n";
        }
        var lines = split(resStr, delim);
        var ret = Response();
        var parseBody = fn(bodyStr){
            if (type(bodyStr) != "string"){
                print_error("bodyStr must be string");
                return;
            }
            if(Strings.isWhitespace(bodyStr)){
                return;
            }
            var m = JSONParser(bodyStr).parse()
            ret.setBody(m);
        }
        if(len(lines)){
            var firstLine = split(lines[0], " ");
            var firstLineLen = len(firstLine);
            if(firstLineLen > 0) ret.setStatusCode(int(firstLine[1]));
            parseBody(lines[-1]);
        }

        return ret;

    }

    toStr(){
        var sb = StringBuilder("Response: { statusCode: ");
        sb.append(this.statusCode);
        if(len(this.body)){
            sb.append(", Body: ")
            .append(this.body.toStr());
        }
        sb.append(" }");
        return sb.toStr();
    }
}

class Path{
    init(){
        this.path = null;
        this.queryParams = HashMap();
    }

    getPath(){
        return this.path;
    }
    setPath(path){
        if(type(path) != "string"){
            print_error("path must be string");
            return null;
        }
        this.path = path;
        return this;
    }
    parse(path){
        var ret = Path();
        var params = split(path, "?");
        ret.setPath(params[0]);
        if(len(params) > 1){
            params = split(params[1], "&");
            for(var i = 0; i < len(params); i+=1){
                var param = split(params[i], "=");
                var val;
                if(len(param) > 1){
                    val = param[1];
                }
                else{
                    val = "";
                }
                ret.queryParams.set(param[0],val);
            }
        }
        return ret;
    }
    toStr(){
        var sb = StringBuilder("Path: { path: \"");
        sb.append(this.path)
        .append("\"");
        if(len(this.queryParams)){
            sb.append(", queryParams: ")
            .append(this.queryParams);
        }
        sb.append(" }");
        return sb.toStr();
    }

}
