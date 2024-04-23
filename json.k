

class JSONParseState{
  init(string, index){
    this.string = string
    this.index = index
  }
  PARSE_ERROR = JSONParseState("ERROR", -1) 

  _eq_(other){
    return this.string == other.string && this.index == other.index
  }

  toStr(){
    return this.string
  }

}

class JSONParser{
  init(string){
    this.string = string.trim();
    this.index = 0;
  }
  parse(){
    this.index = 0;
    var ret = this.parseValue();

    if (this.index < len(this.string)){
      print_error("Error: JSON string is not valid. Extra chars: ${}".f(this.string[this.index:]));
      return JSONParseState.PARSE_ERROR;
    }
    return ret;
  }
  parseValue(){
    var c = this.string[this.index];
    while (c.isWhitespace()){
      this.index += 1;
      if (this.index >= len(this.string)){
        print_error("Error: JSON string is not valid. Ran out of characters while parsing value.");
        return JSONParseState.PARSE_ERROR;
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
          print_error("Error: JSON string is not valid: Failed at character: ${}".f(c));
          return JSONParseState.PARSE_ERROR;
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
          print_error("Error: JSON string is not valid. Ran out of characters while parsing string.");
          return JSONParseState.PARSE_ERROR;
        }
      }
      this.index += 1;
    }
    print_error("Error: JSON string is not valid. Ran out of characters while parsing string.");
    return JSONParseState.PARSE_ERROR;
  }
  parseObject(){
    var ret = HashMap();
    this.index += 1;
    var c;
    while (this.index < len(this.string)){
      if(this.string[this.index].isWhitespace()){ 
        this.index += 1;
        continue;
      }
      c = this.string[this.index];
      if (c == "}"){
        this.index += 1;
        return ret;
      }else if (c == ","){
        this.index += 1;
      }else{
        var key = this.parseString();
        if(ret.containsKey(key)){
          print_error("Error: JSON string is not valid. Duplicate key: '${}'".f(key));
          return JSONParseState.PARSE_ERROR;
        }
        if (key == JSONParseState.PARSE_ERROR){
          return JSONParseState.PARSE_ERROR;
        }
        c = this.string[this.index];
        while (c.isWhitespace()){
          this.index += 1;
          c = this.string[this.index];
        }
        if (c != ":"){
          print_error("Error: JSON string is not valid. Expected ':' but got '${}'".f(c));
          return JSONParseState.PARSE_ERROR;
        }
        this.index += 1;
        var value = this.parseValue();
        if (value == JSONParseState.PARSE_ERROR){
          return JSONParseState.PARSE_ERROR;
        }
        ret[key] = value;

      }
    }
    print_error("Error: JSON string is not valid. Ran out of characters while parsing object.");
    return JSONParseState.PARSE_ERROR;
  }
  parseArray(){
    var ret = [];
    this.index += 1;
    while (this.index < len(this.string)){
      var c = this.string[this.index];
      if(c.isWhitespace()){
        this.index += 1;
        continue;
      }
      if (c == "]"){
        this.index += 1;
        return ret;
      }else if (c == ","){
        this.index += 1;
      }else{
        var value = this.parseValue();
        if (value == JSONParseState.PARSE_ERROR){
          return JSONParseState.PARSE_ERROR;
        }
        ret.append(value);
      }
    }
    print_error("Error: JSON string is not valid. Ran out of characters while parsing array.");
    return null;
  }
  parseTrue(){
    if (this.string[this.index:this.index+4] == "true"){
      this.index += 4;
      return true;
    }
    print_error("Error: JSON string is not valid. Expected 'true' but got '${}'".f(this.string[this.index:this.index+4]));
    return null;
  }
  parseFalse(){
    if (this.string[this.index:this.index+5] == "false"){
      this.index += 5;
      return false;
    }
    print_error("Error: JSON string is not valid. Expected 'false' but got '${}'".f(this.string[this.index:this.index+5]));
    return null;
  }
  parseNull(){
    if (this.string[this.index:this.index+4] == "null"){
      this.index += 4;
      return null;
    }
    print_error("Error: JSON string is not valid. Expected 'null' but got '${}'".f(this.string[this.index:this.index+4]));
    return JSONParseState.PARSE_ERROR;
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
    }else if (type(obj) == "list"){
      ret.append("[");
      for (var i = 0; i < len(obj); i+=1){
        this.stringifyRec(obj[i], ret);
        ret.append(",");
      }
      if (len(obj) > 0) ret.pop()
      ret.append("]");
    }else if (type(obj) == "map"){
        ret.append("{");
        var keys = obj.keys();
        for (var i = 0; i < len(keys); i+=1){
          var key = keys[i];
          this.stringifyRec(str(key), ret);
          ret.append(":");
          this.stringifyRec(obj.get(key), ret);
          ret.append(",");
        }
        if (len(keys) > 0){
          ret.pop();
        }
        ret.append("}");
    }else{
      print_error("JSON.stringify(obj) requires an object that is a string, number, boolean, null, list, or map. Got obj of type: '${}'. Obj: ${}".f(type(obj), obj))
    }
  }
}