if (!ARG_C || ARG_V.indexOf("-u") == -1){
    print "Usage: dotk chatuser.k -u username <optional: -p port>"
    exit(0)
}

import "socket.k"
import "json.k"
import "lang.k"

var port = 8081
var host = "localhost"
if (ARG_V.indexOf("-p") != -1){
    port = ARG_V[ARG_V.indexOf("-p")+1]
}

var username = ARG_V[ARG_V.indexOf("-u")+1]

fn render_chat(data){ #List<HashMap>: [{"usr": "user", "msg": "message"}]
    for(var i = 0; i < len(data); i+=1){
        if(data[i].containsKey("CON")){
            print "${}[${}]${} CONNECTED".f(Ansi.ALL_COLORS[hash( data[i]["CON"]) % len(Ansi.ALL_COLORS)], data[i]["CON"], Ansi.RESET)
            continue
        }
        print "${}[${}]${} ${}".f(Ansi.ALL_COLORS[hash( data[i]["USR"]) % len(Ansi.ALL_COLORS)], data[i]["USR"], Ansi.RESET, data[i]["MSG"])
    }
}


var users = HashMap()
var num_users = 0
var initial_connection = Socket.newSocket()
initial_connection.port = port
initial_connection.host = host
initial_connection.connect()
print "Connected to the server!"
initial_connection.send(`{"CON": "${}"}`.f(username))
var response = initial_connection.receive()
var chat = JSON.parse(response)
render_chat(chat)
initial_connection.close()



var sock = Socket()
while(true){
    # connect to the server
    # send the username and message 
    # receive the message from the server
    # print the message
    try{
        sock = Socket.newSocket()
        sock.port = port
        sock.host = host
        sock.connect()
        if(kbhit()){
            var data = input()
            sock.send(`{"MSG": "${}", "USR": "${}"}`.f(data, username))
            # clear()

        }else{
            # render_chat(chat)
            sock.send(`{"GET": "GET"}`)
        }
        chat = JSON.parse(sock.receive())
        clear()
        render_chat(chat)
        # print("[${}] > ".f(username))
    }catch(e){
        # print(e)
    }
    sleep(0.6);
}