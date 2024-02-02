import "stdlib.k"
import "file.k"

var bf = `>++++++++[<+++++++++>-]<.>++++[<+++++++>-]<+.+++++++..+++.>>++++++[<+++++++>-]<+
+.------------.>++++++[<+++++++++>-]<+.<.+++.------.--------.>>>++++[<++++++++>-
]<+.`

var mem = ArrayList.of(Arrays.fill(100, 0))
var head = 0
var stack = ArrayList()
var sb = StringBuilder()

var size = len(bf)
var i = 0
while(i < size){
  var c = bf[i]
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
      stack.add(i)
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
