import "socket.k"
import "json.k"

var sock = Socket.withDefaults();
sock.bindToPort(8081);
sock.startListening();

var chat = []


fn handleClient(clientSock){
    try{
        var data = clientSock.receive()
        
        var json = JSON.parse(data)
        var json_chat = JSON.stringify(chat)
        if(json.containsKey("GET")){
            print("Received: " + data)
            clientSock.send(json_chat)
        }else{
            chat.append(json)
        }
        clientSock.send(json_chat)
    }catch(e){
        print(e)
    }
}

while (true){
    var client = sock.accept()
    try{
        handleClient(client)
    } catch (e){
        print(e)
    }
    client.close()
}