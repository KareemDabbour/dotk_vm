print("[WARN]: Non-built-in StringBuilder is deprecated. Use the built-in StringBuilder instead. It's a drop in replacement and theres no need to 'import \"StringBuilder.k\"'");
class StringBuilder{
  INITIAL_CAPACITY = 16;
  init(s){
    this.capacity = StringBuilder.INITIAL_CAPACITY;
    this.contents = {};
    if (!s){
      this.size = 0;
    }else if (type(s) != "string"){
        print_error("Error: StringBuiler can only be initialized with a string");
    }else{
      this.contents.append(s);
      this.size = 1;
    }
  }
  append(s){
    if (type(s) != "string"){
      s = str(s);
    }
    this.contents.append(s);
    this.size = this.size + 1;
    return this;
  }

  pop(){
    return this.contents.pop();
  }

  peek(){
    if (this.size == 0){
      print_error("Error: Cannot peek from empty StringBuilder");
    }
    return this.contents[this.size - 1];
  }

  toArray(){
    return this.contents[:this.size];
  }

  toStr(){
    return "".join(this.contents[:this.size]);
  }
  _size_(){
    return this.size;
  }
  new(){
    return StringBuiler(null);
  }
}