class StringBuilder{
  INITIAL_CAPACITY = 16;
  init(s){
    this.capacity = StringBuilder.INITIAL_CAPACITY;
    this.contents = [this.capacity];
    if (!s){
      this.size = 0;
    }else if (type(s) != "string"){
        print_error("Error: StringBuiler can only be initialized with a string");
    }else{
      this.contents[0] = s;
      this.size = 1;
    }
  }
  append(s){
    if (type(s) != "string"){
      s = str(s);
    }
    if (this.size == this.capacity){
      this.resize();
    }
    this.contents[this.size] = s;
    this.size = this.size + 1;
    return this;
  }

  resize(){
    var oldContents = this.contents;
    this.capacity = this.capacity * 2;
    this.contents = [this.capacity];
    for (var i = 0; i < len(oldContents); i+=1){
      this.contents[i] = oldContents[i];
    }
    
  }
  pop(){
    if (this.size == 0){
      print_error("Error: Cannot pop from empty StringBuilder");
    }
    this.size = this.size - 1;
    return this.contents[this.size];
  }

  toArray(){
    return this.contents[:this.size];
  }

  toStr(){
    return join(this.contents[:this.size], "");
  }
  new(){
    return StringBuiler(null);
  }
}