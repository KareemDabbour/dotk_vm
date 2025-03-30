import "socket.k"
import "json.k"

var sock = Socket.withDefaults();
sock.bindToPort(8081);
sock.startListening();

var chat = []
var parent_pipe = pipe()
var chatPipe = pipe()

fn handleClient(clientSock, pid ){
    try{
        var data = clientSock.receive()
    
        # print f"Received: '{data}' '{pid}' type: {type(pid)}"
        if (data == ""){
            clientSock.close()
            return;
        }
        var json = JSON.parse(data)
        # print f"Received: '{json}'"
        # var json_chat = JSON.stringify(chat)
        if(json.containsKey("GET")){
            # var json_chat;
        write(parent_pipe.send, "GET");
                # print f"sent to par: {}"
                # sleep(2)
                # json_chat = read(chatPipe.read)
                # print f"Sending from child: {pid} '{json_chat}'"
                # clientSock.send(json_chat)
            # }catch(e){
                # print(e)
            # }
            # clientSock.send(json_chat)
        }else if (json.containsKey("CON")){
                # print f"sent to par a con: {}"
                write(parent_pipe.send, data)
        }else{
            # chat.append(json)
            write(parent_pipe.send, data)
        }
        # sleep(1)
        var json_chat = read(chatPipe.read)
        # print f"Sending from child: {pid} '{json_chat}'"
        clientSock.send(json_chat)
    }catch(e){
        print(e)
        clientSock.close()
        return;
    }
}

var pid = fork()
if (pid == 0){
    while (true){
        var data = read(parent_pipe.read)
        # print f"got {data}"
        if (data != "GET"){
            chat.append(JSON.parse(data))
        }
        # print f" sent to client: {}"
        write(chatPipe.send, JSON.stringify(chat))
    }
    exit(0)
}

while (true) {
    var client = sock.accept()
    var pid = fork()
    if (pid == 0){
        print f"Accepted connection from {client}"
        # sleep(5)
            try{
                while (!client.isClosed){
                    handleClient(client, pid)
                    sleep(0.5)    
                }
            } catch (e){
                print(e)
            }
        exit(0)
    }

}
wait();

