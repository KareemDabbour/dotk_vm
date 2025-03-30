# if (!ARG_C || ARG_V.indexOf("-u") == -1){
#     print "Usage: dotk chatuser.k -u username <optional: -p port>"
#     exit(0)
# }

import "socket.k"
import "json.k"
import "lang.k"

var port = 8081
var host = "localhost"
var username = f"user{randInt(0, 1000)}"
if (ARG_V.indexOf("-p") != -1){
    port = ARG_V[ARG_V.indexOf("-p")+1]
}

if (ARG_V.indexOf("-u") != -1){
username = ARG_V[ARG_V.indexOf("-u")+1]
}

fn render_chat(data){
    if (len(data) == len(chat)) return;
    
    clear() #List<HashMap>: [{"usr": "user", "msg": "message"}]
    for(var i = 0; i < len(data); i+=1){
        if(data[i].containsKey("CON")){
            print f"{Ansi.ALL_COLORS[hash( data[i]["CON"]) % len(Ansi.ALL_COLORS)]}[{data[i]["CON"]}]{Ansi.RESET} CONNECTED"
            continue
        }
        print "${}[${}]${} ${}".f(Ansi.ALL_COLORS[hash( data[i]["USR"]) % len(Ansi.ALL_COLORS)], data[i]["USR"], Ansi.RESET, data[i]["MSG"])
    }
    chat = data
}


var users = HashMap()
var num_users = 0
var initial_connection = Socket.newSocket()
var chat = []
initial_connection.port = port
initial_connection.host = host

with(initial_connection, fn(in) {
    if (!in.connect()){
        print "Failed to connect to the server"
        return;
    }
    print "Connected to the server!"
    in.send('{"CON": "${}"}'.f(username))
    var response = in.receive()
    render_chat(JSON.parse(response))
    try{
        while(true){
            if(kbhit()){
                var data = input()
                in.send('{"MSG": "' + data + '", "USR": "' + username + '"}')
            }else{
                in.send('{"GET": "GET"}')
            }
            sleep(0.6)
            
            # clear()

            render_chat(JSON.parse(in.receive()))
        }
    }catch(e){
        print(f"ERROR: disconnected from server")
    }

})




# var sock = Socket()
# while(true){
#     # connect to the server
#     # send the username and message 
#     # receive the message from the server
#     # print the message
#     try{
#         sock = Socket.newSocket()
#         sock.port = port
#         sock.host = host
#         sock.connect()
#         if(kbhit()){
#             var data = input()
#             sock.send(f"\{\"MSG\": \"{data}\", \"USR\": \"{username}\"\}")
#             # clear()

#         }else{
#             # render_chat(chat)
#             sock.send(`{"GET": "GET"}`)
#         }
#         chat = JSON.parse(sock.receive())
#         clear()
#         render_chat(chat)
#         # print("[${}] > ".f(username))
#     }catch(e){
#         # print(e)
#     }
#     sleep(0.6);
# }