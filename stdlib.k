import "StringBuilder.k";
## A simple linked list implementation.
class ListNode {
  init(value) {
    this.value = value;
    this.next = null;
    this.previous = null;
  }
}
#TODO FIX ITERATOR!!!!
class Iterable{
  iterator(){
    class Iterator{
      init(x){
        this.i = -1;
        this.stuff = x;
      }
      hasNext(){
        return (this.i + 1) < this.stuff.size()
      }
      next(){
        if (!this.hasNext()){
          print_error("Iterator has no next element")
        }
        this.i = this.i + 1;
        return this.stuff.getI(this.i);
      }
      get(){
        return this.stuff.getI(this.i);
      }
      toStr(){
        var ret = StringBuilder("{ \"i\": ");
        ret.append(this.i).append(", \"stuff\": ").append(this.stuff.toStr()).append("}");
        return ret.toStr();
      }

    }
    return Iterator(this);
  }
}

class Collection <Iterable> {
  init(){
    print_error("Cannot instantiate abstract class Collection");
  }
}

class LinkedList <Iterable> {

  init() {
    this.head = null;
    this.tail = null;
    this._length = 0;
  }

  # Append value to the end of the list.
  append(value) {
    var node = ListNode(value);
    if (this.head == null) {
      this.head = node;
      this.tail = node;
    } else {
      node.previous = this.tail;
      this.tail.next = node;
      this.tail = node;
    }
    this._length = this._length + 1;
    return this;
  }

  # Pop the last value off the list && return it. Do nothing if
  # list if empty;
  pop() {
    if (this.tail == null) {
      return;
    }
    var node = this.tail;
    var previous = node.previous;
    this.tail = previous;
    if (previous != null) {
      previous.next = null;
    } else {
      this.head = null;
    }
    this._length = this._length - 1;
    return node.value;
  }

  map(f){
    var node = this.head;
    while (node != null){
        node.value = f(node.value);
        node = node.next;
    }
  }

  # Call f(value) for each value in the list, from first value
  # appended to last. If f returns a truthy value, stop iterating.
  foreach(f) {
    var node = this.head;
    while (node != null) {
      if (f(node.value)) {
        return;
      }
      node = node.next;
    }
  }

  findFirst(f){
    var node = this.head;
    while (node != null){
        if (f(node.value)){
            return node.value;
        }
        node = node.next;
    }
    return null;
  }

  removeIf(f){
    var node = this.head;
    while (node != null){
        if (f(node.value)){
            this.removeNode(node);
            return node.value;
        }
        node = node.next;
    }
    return null;
  }

  removeNode(node){
    if (node == null){
      return;
    }
    var previous = node.previous;
    var next = node.next;
    if (previous != null){
      previous.next = next;
    } else {
      this.head = next;
    }
    if (next != null){
      next.previous = previous;
    } else {
      this.tail = previous;
    }
    this._length = this._length - 1;
  }

  # Get the nth (0-based) element of the list. Because it's a linked
  # list (Lox doesn't have arrays), this is an O(n) operation.
  get(n) {
    var node = this.head;
    var i = 0;
    while (node != null) {
      if (i == n) {
        return node.value;
      }
      node = node.next;
      i = i + 1;
    }
    return null;
  }
  
  getI(i){
    return this.get(i);
  }

  # Return the length of the list. The length is tracked as the list
  # is appended to, so this is an O(1) operation.
  size() {
    return this._length;
  }

  # Return the last element, or null if list if empty.
  last() {
    if (this.tail != null) {
      return this.tail.value;
    }
    return null;
  }

  contains(obj){
    var node = this.head;
    while (node != null){
        if (node.value == obj){
            return true;
        }
        node = node.next;
    }
    return false;
  }
  toStr(){
    var ret = StringBuilder("[");
    var node = this.head;
    while (node != null){
        ret.append(node.value);
        node = node.next;
        if (node != null){
            ret.append(", ");
        }
    }
    ret.append("]");
    return ret.toStr();
  }
}

~ 
Helper functions for working with lists.
~
class Arrays{
  mergeSort(arr){
    if(len(arr) <= 1){
        return arr;
    }
    var mid = len(arr) // 2;
    var left = arr[:mid];    
    var right = arr[mid:];
    this.mergeSort(left);
    this.mergeSort(right);
    this.merge(arr, left, right);
  }
  merge(arr, left, right) {
    var i = 0;
    var j = 0;
    var index = 0;

    while((i < len(left)) && (j < len(right))){
        if(left[i] < right[j]){
            arr[index] = left[i];
            i += 1;
        }else{
            arr[index] = right[j];
            j += 1;
        }
        index += 1;
    }
    while (i < len(left)){
        arr[index] = left[i];
        i += 1;
        index += 1;
    }
    while (j < len(right)){
        arr[index] = right[j];
        j += 1;
        index += 1;
    }
  }
  nDList(dimensionArray, defaultVal){
    var ret;
    for (var i = 0; i<len(dimensionArray); i+=1){
        if (type(dimensionArray[i]) != "int"){
          print_error("Error: dimensionArray must be an array of integers");
        } else if (dimensionArray[i] <= 0 ){
          print_error("Error: dimensionArray must not contain any 0 or negative integers"); 
        }
    }
    ret = this.nDRec(dimensionArray, defaultVal);
    return ret;
  }
  nDRec(dList, dVal){
    if (len(dList) == 1){
      return [dList[0]]{dVal};
    }
    var ret = [dList[0]]{dVal};
    for (var i = 0; i<len(ret); i+=1){
      ret[i] = this.nDRec(dList[1:], dVal);
    }
    return ret;
  }

  map(arr, f){
    for (var i = 0; i < len(arr); i+=1){
      arr[i] = f(arr[i]);
    }
  }
  reduce(arr, f){
    return ArrayList.of(arr).removeIf(f).toArray();
  }
}

class Math{
  PI = 3.14;  
  e = 2.71828;
}

fn max(a, b){
  if (a > b){
    return a;
  }
  return b;
}
fn min(a, b){
  if (a < b){
    return a;
  }
  return b;
}
fn foreach(iterable, forFunc){
  if(!instanceof(iterable, Iterable)){
    print_error("foreach(iterable, forFunc) requires an Iterable as its first argument");
    return;
  }
  if (type(forFunc) != "function"){
    print_error("foreach(iterable, forFunc) requires a function as its second argument" );
    return;
  }
  var iter = iterable.iterator();
  while(iter.hasNext()){
    var ret = forFunc(iter.next());
    if (ret != null && !ret){
      break;
    } 
  }
    # for(var i = iterable.iterator();i.hasNext();i.next()){
    #   var ret = forFunc(i.get());
    #   if (ret != null && !ret){
    #     break;
    #   }
    # }
}

## A map implemented as a hash table with separate chaining.

class MapEntry{
  init(key, value, next){
    this.key = key;
    this.value = value;
  }
  toStr(){
    return this.key + ":" + this.value;
  }
  lt(other){
    return this.value < other.value
  }
  gt(other){
    return this.value > other.value
  }

}

fn hashKey(key, capacity){
  var ret = hashStr(key);
  return (ret % capacity);
}

class Map <Iterable>{
  INITIAL_CAPACITY = 32;
  GROW_FACTOR = 2;
  MAX_LOAD_FACTOR = 0.75;
  init(){
    this.capacity = Map.INITIAL_CAPACITY;
    this.keys = Set();
    this.entries = Set();
    this.entries = [this.capacity]{null};
    this._size = 0;
  }
  getI(i){
    return this.get(this.keys.getI(i));
  }
  get(key){
    var hash = hashKey(key, this.capacity);
    var entryList = this.entries[hash];
    if (entryList == null){
      return null;
    }
    var ret = entryList.findFirst(fn (other) => key == other.key);
    
    if (ret != null){
      return ret.value;
    }
    
    return null;
  }
  getOrDefault(key, defaultValue){
    var ret = this.get(key);
    if (ret == null){
      return defaultValue;
    }
    return ret;
  }
  put(key, value){
    var hash = hashKey(key, this.capacity);
    var entryList = this.entries[hash];
    if(entryList == null){
      var entry = LinkedList();
      entry.append(MapEntry(key, value, null));
      this.entries[hash] = entry;
    }else{
      var node = entryList.head;
      while(node != null){
        if (node.value.key == key){
          node.value.value = value;
          return value;
        }
        node = node.next;
      }
      entryList.append(MapEntry(key, value, null));
    }
    this._size = this._size + 1; 
    this.keys.add(key); 

    if (((this._size) / this.capacity) >= Map.MAX_LOAD_FACTOR){
      this.resize();
    }
    return value;
  }
  remove(key){
    var hash = hashKey(key, this.capacity);
    var entryList = this.entries[hash];
    if(entryList == null){
      return null;
    }
    var ret = entryList.removeIf(fn (other){return key == other.key;});
    if (ret != null){
      this._size = this._size - 1;
      this.keys.remove(key);
    }
    return ret;
  }
  resize(){
    var oldValues = this.entries;
    this.capacity = this.capacity * Map.GROW_FACTOR;
    this.entries = [this.capacity];
    this._size = 0;
    for (var i = 0; i < len(oldValues); i+=1){
      var node = oldValues[i];
      while (node != null){
        print node;
        this.put(node.key, node.value);
        node = node.next;
      }
    }
  }
  of(entryList){
    if (len(entryList)%2 != 0){
        print_error("Cannot create map from odd number of key value pairs");
        return null;
    }

    var keys = entryList[:@2];
    for (var i = 0; i < len(keys); i+=1){
      if (type(keys[i]) != "string"){
        keys[i] = str(keys[i]);
      }
    }
    var values = entryList[1:@2];

    var ret = this.ofSize(max(len(entryList) * 2, Map.INITIAL_CAPACITY));
    for (var i = 0; i < len(keys); i+=1){
      ret.put(keys[i], values[i]);
    }
    return ret;
  }
  ofSize(s){
    var ret = Map();
    ret.capacity = s;
    ret.entries = [s]{null};
    return ret;
  }
  toStr(){
    var ret = StringBuilder("{");
    # var ret = "Map:{ entries: [";
    if(this._size > 0){
      var cap = this.capacity;
      var appendEntry = fn (entry){
        ret.append(entry.toStr()).append(", ");
      };

      for (var i = 0; i < cap; i+= 1){
        # entryList = this.entries[i];
        var entryList = this.entries[i];
        if(entryList != null){
          entryList.foreach(appendEntry);
        }
        
      }
      ret.pop();
    }
    ret.append("}");
    # ret += "], size: " + str(this._size) + ", capacity: " + str(this.capacity) + "}";
    # return ret;    
    return ret.toStr();    
  }
  size(){
    return this._size;
  }
  isEmpty(){
    return this._size == 0;
  }
}

class Set <Iterable>{
  INITIAL_CAPACITY = 16;
  GROW_FACTOR = 2;
  MAX_LOAD_FACTOR = 0.75;
  init(){
    this.capacity = Set.INITIAL_CAPACITY;
    this.entries = [this.capacity];
    this.allEntries = [this.capacity];
    this._size = 0;
  }

  add(value){
    if(type(value) == "object" && instanceof(value, Set)){
      if(value == this)
        return this;
      else{
        var set = this;
        foreach(value, fn (x){set.add(x);});
      }
    }

    if(this._size >= this.capacity * Set.MAX_LOAD_FACTOR){
      this.resize();
    }

    var hash = hashKey(value, this.capacity);
    var entryList = this.entries[hash];
    if(entryList == null){
      var entry = LinkedList();
      entry.append(value);
      this.entries[hash] = entry;
      # print "needed new list for " + str(value);
    }else if (entryList.contains(value)){
      # print "already in";
      return value;
    }else{
      # print "appended new value: " + str(value);
      entryList.append(value);
    }
    this.allEntries[this._size] = value;
    this._size = this._size + 1;
    return value;
  }

  getI(i){
    if (i >= this._size || i < 0){
      print_error("Index out of bounds");
      return null;
    }
    return this.allEntries[i];
  }

  size(){
    return this._size;
  }

  resize(){

    var oldAllValues = this.allEntries;
    var oldSize = this._size;
    this.allEntries = this.allEntries + [this.capacity];
    # print "resizing from " + str(this.capacity) + " to " + str(this.capacity * Set.GROW_FACTOR);
    this.capacity = this.capacity * Set.GROW_FACTOR;
    this.entries = [this.capacity];
    # this.allEntries = [this.capacity];
    this._size = 0;

    for (var i = 0; i < oldSize; i+=1){
      var val = oldAllValues[i];
      this.add(val);
    }
  }

  contains(value){
    if (this._size == 0){
      return false;
    }
    if(type(value) == "object" && instanceof(value, Set)){
      if(value == this)
        return true;
      else{
        var ret = true;
        var set = this;
        foreach(value, fn (x){
          if (!set.contains(x)){
            ret = false;
            return false; # breaks out of the foreach loop 
          }
        });
        return ret;
      }
    }
    var hash = hashKey(value, this.capacity);
    var entryList = this.entries[hash];
    if(entryList == null){
      return false;
    }
    return entryList.contains(value);
  }

  remove(value){
    var hash = hashKey(value, this.capacity);
    var entryList = this.entries[hash];
    if(entryList == null){
      return null;
    }
    var ret = entryList.removeIf(fn (other){return value == other;});
    if (ret != null){
      this._size = this._size - 1;
    }
    return ret;
  }

  intersection(otherSet){
    var ret = Set();
    for (var i = 0; i < this._size; i+=1){
      var val = this.allEntries[i];
      if (otherSet.contains(val)){
        ret.add(val);
      }
    }
    return ret;
  }

  difference(otherSet){
    var ret = Set();
    for (var i = 0; i < this._size; i+=1){
      var val = this.allEntries[i];
      if (!otherSet.contains(val)){
        ret.add(val);
      }
    }
    return ret;
  }

  of(arr){
    var ret = Set();
    ret.capacity = max(len(arr) * 2, Set.INITIAL_CAPACITY);
    ret.entries = [ret.capacity];
    ret.allEntries = [ret.capacity];
    for (var i = 0; i < len(arr); i+=1){
      ret.add(arr[i]);
    }
    return ret;
  }

  toStr(){  
    return str(this.allEntries[:this._size]);
  }
}

class ArrayList <Iterable> {
  INITIAL_CAPACITY = 32;
  init(){
    this.capacity = ArrayList.INITIAL_CAPACITY;
    this._size = 0;
    this.entries = [this.capacity]; 
  }

  get(i){
    if (i >= this._size){
      print_error("Index out of bounds");
      return null;
    }
    return this.entries[i];
  }

  getLast(){
    if (this._size == 0){
      # print_error("Cannot get last element of empty list");
      return null;
    }
    return this.entries[this._size - 1];
  }

  set(i, value){
    if (i >= this._size){
      print_error("Index out of bounds");
      return null;
    }
    this.entries[i] = value;
    return value;
  }

  add(value){
    if (this._size >= this.capacity){
      this.resize();
    }
    this.entries[this._size] = value;
    this._size = this._size + 1;
    return value;
  }

  addAll(values){
    for (var i = 0; i < len(values); i+=1){
      this.add(values[i]);
    }
    # this.entries = this.entries[:this._size] + values;
    # this._size = this._size + len(values);
    # this.capacity = this.capacity + len(values);
    return values;
  }

  addAtIndex(i, value){
    if (i > this._size){
      print_error("Index out of bounds");
      return null;
    }
    if (this._size >= this.capacity){
      this.resize();
    }
    for (var j = this._size; j > i; j-=1){
      this.entries[j] = this.entries[j-1];
    }
    this.entries[i] = value;
    this._size = this._size + 1;
    return value;
  }
  removeIf(f){
    var i = 0;
    while (i < this._size){
      if (f(this.entries[i])){
        this.remove(i);
      } 
      i += 1; 
    }
    return this;
  }
  remove(i){
    if (i >= this._size){
      print_error("Index out of bounds");
      return null;
    }
    var ret = this.entries[i];
    for (var j = i; j < this._size - 1; j+=1){
      this.entries[j] = this.entries[j+1];
    }
    this.entries[this._size - 1] = null;
    this._size = this._size - 1;
    return ret;
  }

  clear(){
    this.capacity = ArrayList.INITIAL_CAPACITY;
    this._size = 0;
    this.entries = [this.capacity];
  }

  pop(){
    if (this._size == 0){
      print_error("Cannot pop from empty list");
      return null;
    }
    this._size = this._size - 1;
    return this.entries[this._size];
  }

  resize(){
    this.capacity = this.capacity * 2;
    var newEntries = [this.capacity];
    for (var i = 0; i < this._size; i+=1){
      newEntries[i] = this.entries[i];
    }
    this.entries = newEntries;
  }

  of(arr){
    if(type(arr) != "list"){
      print_error("ArrayList.of(arr) takes in an array");
      return null;
    }
    var ret = ArrayList();
    ret.capacity = max(len(arr) * 2, ArrayList.INITIAL_CAPACITY);
    ret._size = len(arr);
    ret.entries = [ret.capacity];
    for (var i = 0; i < len(arr); i+=1){
      ret.entries[i] = arr[i];
    }
    return ret;
  }

  toArray(){
    return this.entries[:this._size];
  }

  size(){
    return this._size;
  }

  isEmpty(){
    return this._size == 0;
  }

  forEach(f){
    if (type(f) != "function"){
      print_error("Error: forEach requires a function as an argument but the following type was passed in: " + type(f) );
      return;
    }
    for (var i = 0; i < this._size; i+=1){
      f(this.entries[i]);
    }
  }

  sort(){
    var res = this.entries[:this._size];
    Arrays.mergeSort(res);
    for(var i = 0; i < len(res); i+=1){
      this.entries[i] = res[i];
    }
    return this;
  }

  toStr(){
    return this.entries[:this._size];
  }

  eq(other){
    if (type(other) != "object" || !instanceof(other, ArrayList)){
      return false;
    }

    if (other.size() != this._size){
      return false;
    }

    for (var i = 0; i < this._size; i+=1){
      if (this.entries[i] != other.get(i)){
        return false;
      }
    }
    return true;
  }

  contains(obj){
    for (var i = 0; i < this._size; i+=1){
      if (this.entries[i] == obj){
        return true;
      }
    }
    return false;
  }
}

class Strings{
  WHITE_SPACE = Set.of(list(" \n\t\r"));
  containsChar(str, c){
    for (var i = 0; i < len(str); i+=1){
      if (str[i] == c){
        return true;
      }
    }
    return false;
  }
  contains(str, substr){
    if (len(substr) > len(str)){
      return false;
    }
    var strLen = len(str);
    var substrLen = len(substr);
    if(substrLen == 0){
      return true;
    }
    if(substrLen == 1){
      return this.containsChar(str, substr);
    }
    for (var i = 0; i < strLen; i+=1){
      if (str[i] == substr[0]){
        var match = true;
        var j = 0;
        while(j < substrLen){
          if (i+j >= strLen || str[i+j] != substr[j]){
            match = false;
            break;
          }
          j+=1;
        }
        if (match){
          return true;
        }
      }
    }
    return false;
  }
  findFirst(str, substr){
    if (len(substr) > len(str)){
      return -1;
    }
    var strLen = len(str);
    var substrLen = len(substr);
    for (var i = 0; i < strLen; i+=1){
      if (str[i] == substr[0]){
        var match = true;
        var j = 0;
        while(j < substrLen){
          if (i+j >= strLen || str[i+j] != substr[j]){
            match = false;
            break;
          }
          j+=1;
        }
        if (match){
          return i;
        }
      }
    }
    return -1;
  }
  splitByChar(str, delim){
    var ret = ArrayList();
    var strLen = len(str);
    var delimLen = len(delim);
    if(delimLen > 1){
      print_error("splitByChar(str, char) only accepts a single character delimiter");
      return ret.toArray();
    }
    if (delimLen > strLen){
      ret.add(str);
    }else{
      var start = 0;
      for(var i = 0; i <strLen; i+=1){
        if (str[i] == delim){
          ret.add(str[start:i]);
          start = i+1;
        }
      }
      ret.add(str[start:]);
    }
    return ret.toArray();
  }
  split(str, delim){
    var ret = ArrayList();
    var strLen = len(str);
    var delimLen = len(delim);
    if(delimLen == 1){
      return this.splitByChar(str, delim);
    }
    if (delimLen > strLen){
      ret.add(str);
    }else{
      var start = 0;
      for(var i = 0; i < strLen; i+=1){
        if ( str[i:i + delimLen] == delim){
          ret.add(str[start:i]);
          start = i+delimLen;
        }
      }
      ret.add(str[start:]);
    }
    return ret.toArray();
  }
  isWhitespace(string){
    for (var i = 0; i < len(string); i+=1){
      if (!this.WHITE_SPACE.contains(string[i])){
        return false;
      }
    }
    return true;
  }
  hasWhitespace(string){
    if(type(string) != "string"){
      print_error("hasWhitespace(string) requires a string as its argument");
      return false;
    }
    for (var i = 0; i < len(string); i+=1){
      if (this.WHITE_SPACE.contains(string[i])){
        return true;
      }
    }
    return false;
  }
  trim(string){
    var start = 0;
    var end = len(string) - 1;
    while (start < len(string) && this.WHITE_SPACE.contains(string[start])){
      start += 1;
    }
    while (end >= 0 && this.WHITE_SPACE.contains(string[end])){
      end -= 1;
    }
    return string[start:end+1];
  }
  trimLeft(string){
    var start = 0;
    while (start < len(string) && this.WHITE_SPACE.contains(string[start])){
      start += 1;
    }
    return string[start:];
  }
  trimRight(string){
    var end = len(string) - 1;
    while (end >= 0 && this.WHITE_SPACE.contains(string[end])){
      end -= 1;
    }
    return string[:end+1];
  }
}


