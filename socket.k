import "stdlib.k";

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
    SO_TYPE = 3;
    init(){
        this.socket = null;
        this.port = null;
    }
    bindToPort(port){
        this.port = port;
        return bind(this.socket, port);
    }
    setDefaultSocketOptions(){
        setSockOpt(this.socket, Socket.SOL_SOCKET, Socket.SO_REUSEADDR);
    }
    setSocketOptions(level, option){
        if(type(level) != "int" || type(option) != "int"){
            print_error("level and option must be int");
        }
        setsockopt(this.socket, level, option);
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
    equals(other){  
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
        this.body = Map();
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
        if (!body && type(body) != "Map"){
            print_error("body must be a Map");
            return null;
        }
        this.body = body;
        return this;
    }
    parse(reqString){
        var delim = "\n";
        if(Strings.contains(reqString, "\r")){
            delim = "\r\n";
        }
        var lines = Strings.split(reqString, delim);
        var ret = Request();
        if(len(lines)){

            var firstLine = Strings.split(lines[0], " ");
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
        if(!this.body.isEmpty()){
            if(this.path)
                sb.append(", ");
            sb.append("Body: ")
            .append(this.body.toStr());
        }
        sb.append(" }");
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
        var params = Strings.split(body, "&");
        for(var i = 0; i < len(params); i+=1){
            var param = Strings.split(params[i], "=");
            var val;
            if(len(param) > 1){
                val = param[1];
            }
            else{
                val = "";
            }
            this.body.put(param[0],val);
        }
        return this;
    }
}

class Path{
    init(){
        this.path = null;
        this.queryParams = Map();
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
        var params = Strings.split(path, "?");
        ret.setPath(params[0]);
        if(len(params) > 1){
            params = Strings.split(params[1], "&");
            for(var i = 0; i < len(params); i+=1){
                var param = Strings.split(params[i], "=");
                var val;
                if(len(param) > 1){
                    val = param[1];
                }
                else{
                    val = "";
                }
                ret.queryParams.put(param[0],val);
            }
        }
        return ret;
    }
    toStr(){
        var sb = StringBuilder("Path: { path: \"");
        sb.append(this.path)
        .append("\"");
        if(!this.queryParams.isEmpty()){
            sb.append(", queryParams: ")
            .append(this.queryParams.toStr());
        }
        sb.append(" }");
        return sb.toStr();
    }

}
