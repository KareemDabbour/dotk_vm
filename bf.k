import "lang.k"
var bf = `>++++++++[<+++++++++>-]<.>++++[<+++++++>-]<+.+++++++..+++.>>++++++[<+++++++>-]<++.------------.>++++++[<+++++++++>-]<+.<.+++.------.--------.>>>++++[<++++++++>-]<+.`
var interpret = false 
if (ARG_C){
  if (ARG_V.contains("-h")){
    print("Usage: bf [-f file | -s string | -i interpreter]")
    print("If nothing is provided, 'Hello World!' will be printed")
    exit(0)
  }
  
  for (var i = 0; i < ARG_C; i += 1){
    if (ARG_V[i] == "-f"){
      if((i + 1) < ARG_C){
        bf = with(File(ARG_V[i + 1]), fn(f) => f.read())
        break;
      }
      print("Error: -f requires a file name")
      exit(1)
    } else if (ARG_V[i] == "-s"){
      if((i + 1) < ARG_C){
        bf = ARG_V[i + 1]
        break;
      }
      print("Error: -s requires a string")
      exit(1)
    }else if(ARG_V[i] == "-i"){
      interpret = true
      bf = ""
      break;
    }
  }
}

fn run(bf){
  var size = len(bf)
  var i = 0
  var c = ""
  var mem = [1000]{0}
  var head = 0
  var stack = {}
  while(i < size){
    c = bf[i]  
    if (c == ">"){
      head += 1
    }else if (c == "<"){
      head -= 1
    }else if (c == "+"){
      mem[head] = mem[head] + 1
    }else if (c == "-"){
      mem[head] = mem[head] - 1
    }else if (c == "."){
      write(STDOUT, chr(mem[head]))
    }else if (c == ","){
      # print "Enter a character: "
      mem[head] = getc()
    }else if (c == "["){
      if (mem[head] == 0){
        var count = 1
        while(count > 0){
          i += 1
          if (bf[i] == "[") count += 1
          else if (bf[i] == "]") count -= 1
        }
      }else{
        stack.append(i)
      }
    }else if (c == "]"){
      if (mem[head] != 0){
        i = stack[-1]
      }else{
        stack.pop()
      }
    }
    i += 1
  }
}


var STDOUT = 1
if(interpret) {
  while(true){
    try{
      bf = input(Ansi.CYAN + "bf> " + Ansi.RESET).trim()
      if((bf.contains("quit")) || (bf.contains("exit"))) 
        exit(0)
      run(bf)
    }
    catch(e){
      print Ansi.RED+"OOPS... ran into an error! Try again. ${}".f(e) + Ansi.RESET
    }
  }
}
run(bf)

