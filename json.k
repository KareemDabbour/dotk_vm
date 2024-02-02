import "stdlib.k"

class JSONParser{
  init(string){
    this.string = string;
    this.index = 0;
  }
  parse(){
    this.index = 0;
    var ret = this.parseValue();
    if (this.index < len(this.string)){
      print_error("Error: JSON string is not valid");
      return null;
    }
    return ret;
  }
  parseValue(){
    var c = this.string[this.index];
    while (Strings.isWhitespace(c)){
      this.index += 1;
      if (this.index >= len(this.string)){
        print_error("Error: JSON string is not valid");
        return null;
      }
      c = this.string[this.index];
    }
    switch(c){

      case ("\""){
        return this.parseString();
      }
      case ("{"){
        return this.parseObject();
      }
      case ("["){
        return this.parseArray();
      }
      case ("t"){
        return this.parseTrue();
      }
      case ("f"){
        return this.parseFalse();
      }
      case ("n"){
        return this.parseNull();
      }

      default {
        if (c == "-" || (c >= "0" && c <= "9")){
          return this.parseNumber();
        }else{
          print_error("Error: JSON string is not valid");
          return null;
        }
      }
    }
  }
  parseString(){
    var start = this.index + 1;
    this.index += 1;
    while (this.index < len(this.string)){
      var c = this.string[this.index];
      if (c == "\""){
        this.index += 1;
        return this.string[start:this.index-1];
      }else if (c == "\\"){
        this.index += 1;
        if (this.index >= len(this.string)){
          print_error("Error: JSON string is not valid");
          return null;
        }
      }
      this.index += 1;
    }
    print_error("Error: JSON string is not valid");
    return null;
  }
  parseObject(){
    var ret = Map();
    this.index += 1;
    while (this.index < len(this.string)){
      if(Strings.isWhitespace(this.string[this.index])){
        this.index += 1;
        continue;
      }
      var c = this.string[this.index];
      if (c == "}"){
        this.index += 1;
        return ret;
      }else if (c == ","){
        this.index += 1;
      }else{
        var key = this.parseString();
        if (key == null){
          return null;
        }
        c = this.string[this.index];
        if (c != ":"){
          print_error("Error: JSON string is not valid");
          return null;
        }
        this.index += 1;
        var value = this.parseValue();
        if (value == null){
          return null;
        }
        ret.put(key, value);
      }
    }
    print_error("Error: JSON string is not valid");
    return null;
  }
  parseArray(){
    var ret = ArrayList();
    this.index += 1;
    while (this.index < len(this.string)){
      var c = this.string[this.index];
      if (c == "]"){
        this.index += 1;
        return ret;
      }else if (c == ","){
        this.index += 1;
      }else{
        var value = this.parseValue();
        if (value == null){
          return null;
        }
        ret.add(value);
      }
    }
    print_error("Error: JSON string is not valid");
    return null;
  }
  parseTrue(){
    if (this.string[this.index:this.index+4] == "true"){
      this.index += 4;
      return true;
    }
    print_error("Error: JSON string is not valid");
    return null;
  }
  parseFalse(){
    if (this.string[this.index:this.index+5] == "false"){
      this.index += 5;
      return false;
    }
    print_error("Error: JSON string is not valid");
    return null;
  }
  parseNull(){
    if (this.string[this.index:this.index+4] == "null"){
      this.index += 4;
      return null;
    }
    print_error("Error: JSON string is not valid");
    return null;
  }
  parseNumber(){
    var start = this.index;
    var c = this.string[this.index];
    if (c == "-"){
      this.index += 1;
    }
    while (this.index < len(this.string)){
      c = this.string[this.index];
      if (c >= "0" && c <= "9"){
        this.index += 1;
      }else{
        break;
      }
    }
    if (this.index < len(this.string) && this.string[this.index] == "."){
      this.index += 1;
      while (this.index < len(this.string)){
        c = this.string[this.index];
        if (c >= "0" && c <= "9"){
          this.index += 1;
        }else{
          break;
        }
      }
    } else if (this.index < len(this.string) && (this.string[this.index] == "e" || this.string[this.index] == "E")){
      this.index += 1;
      if (this.index < len(this.string) && (this.string[this.index] == "+" || this.string[this.index] == "-")){
        this.index += 1;
      }
      while (this.index < len(this.string)){
        c = this.string[this.index];
        if (c >= "0" && c <= "9"){
          this.index += 1;
        }else{
          break;
        }
      }
    }else{
      return int(this.string[start:this.index]);
    }
    return float(this.string[start:this.index]);
  }
}

class JSON{
  parse(string){
    var parser = JSONParser(string);
    return parser.parse();
  }
  stringify(obj){
    var ret = StringBuilder();
    this.stringifyRec(obj, ret);
    return ret.toStr();
  }
  stringifyRec(obj, ret){
    if (type(obj) == "string"){
      ret.append("\"").append(obj).append("\"");
    }else if (type(obj) != "list" && type(obj) != "object"){
      ret.append(obj);
    }else if (type(obj) == "list"){
      ret.append("[");
      for (var i = 0; i < len(obj); i+=1){
        this.stringifyRec(obj[i], ret);
        ret.append(",");
      }
      ret.pop();
      ret.append("]");
    }else if (type(obj) == "object"){
      if(instanceof(obj, Map)){
        ret.append("{");
        var entrySet = obj.entrySet
        for (var i = 0; i< len(entrySet); i+=1){
          var entry = entrySet[i];
          ret.append("\"");
          this.stringifyRec(entry.key, ret);
          ret.append("\":");
          this.stringifyRec(entry.value, ret);
          ret.append(",");
        }
        if (len(entrySet) > 0){
          ret.pop();
        }
        ret.append("}");

      }else if(instanceof(obj, Iterable)){
        ret.append("[");
        for (var i = 0; i < len(obj); i+=1){
          this.stringifyRec(obj[i], ret);
          ret.append(",");
        }
        if(len(obj) > 0){
          ret.pop();
        }
        ret.append("]");
      }
    }else{
      print_error("JSON.stringify(obj) requires an object that is a string, number, boolean, null, list, or map");
    }
  }
}