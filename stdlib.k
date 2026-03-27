import "lang.k"
## A simple linked list implementation.
class ListNode {
  init(value) {
    this.value = value;
    this.next = null;
    this.previous = null;
  }

  _lt_(other){
    return this.value < other.value
  }
  _gt_(other){
    return this.value > other.value
  }
  _eq_(other){
    if(!instanceof(other, ListNode)){
      return false;
    }
    return this.value == other.value
  }
  toStr(){
    return this.value;
  }
}

class SortOrder{
  ASC = "asc";
  DESC = "desc";

  init(){
    print_error("Cannot instantiate Enum SortOrder");
  }
}

class Iterable{
  iterator(){
    class Iterator{
      init(x){
        this.i = -1;
        this.stuff = x;
      }
      hasNext(){
        return (this.i + 1) < len(this.stuff)
      }
      next(){
        if (!this.hasNext()){
          print_error("Iterator has no next element")
        }
        this.i += 1;
        return this.stuff[this.i];
      }
      get(){
        return this.stuff[this.i];
      }
      toStr(){
        var ret = StringBuilder("{ \"i\": ");
        ret.append(this.i).append(", \"stuff\": ").append(this.stuff.toStr()).append("}");
        return ret.toStr();
      }

    }
    return Iterator(this);
  }
  foreach(f){
    for (var i = 0; i < len(this); i+=1){
      var ret = f(this[i])
      if (ret != null && ret){
        return ret;
      }
    }
  }
  enumerate(f){
    for (var i = 0; i < len(this); i+=1){
      var ret = f(i, this[i])
      if (ret != null && ret){
        return ret;
      }
    }
  }
  map(f){
    for (var i = 0; i < len(this); i+=1){
      this[i] = f(this[i]);
    }
    return this;
  }
  reduce(reduceFn, initialVal){
    var ret = initialVal;
    this.foreach(fn (x){
      ret = reduceFn(ret, x);
    });
    return ret;
  }
  max(){
    return Arrays.max(this);
  }
  min(){
    return Arrays.min(this);
  }
  _eq_(other){
    if(other == null){
      return false;
    }
    if (type(other) != "list" && (type(other) == "object" && !instanceof(other, Iterable))){
    # if the other object is not an iterable or a list, then it is not equal to this iterable
    
      return false;
    }

    if (len(other) != len(this)){
      print "not equal length" + len(other) + " " + len(this);
      return false;
    }

    for (var i = 0; i < len(this); i+=1){
      if (this[i] != other[i]){
        return false;
      }
    }
    return true;
  }
}

class Collection <Iterable> {
  init(){
    print_error("Cannot instantiate abstract class Collection");
  }
}

class LinkedList <Collection> {

  init() {
    this.head = null;
    this.tail = null;
    this._length = 0;
    this.get = this._get_;
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
    # this._length = this._length + 1;
    this._length += 1;
    return this;
  }
  # add to the beginning of the list
  prepend(value){
    var node = ListNode(value);
    if (this.head == null) {
      this.head = node;
      this.tail = node;
    } else {
      node.next = this.head;
      this.head.previous = node;
      this.head = node;
    }
    # this._length = this._length + 1;
    this._length += 1;
    return this;
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
    # this._length = this._length - 1;
    this._length -= 1;
  }

  # Get the nth (0-based) element of the list. Because it's a linked
  # list (Lox doesn't have arrays), this is an O(n) operation.
  _get_(n){
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

  # Return the length of the list. The length is tracked as the list
  # is appended to, so this is an O(1) operation.
  _size_() {
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

  of(arr){
    var ret = LinkedList();
    for (var i = 0; i < len(arr); i+=1){
      ret.append(arr[i]);
    }
    return ret;
  }
}


class SortedLinkedList <LinkedList>{
  # init(){
  #   this.head = null;
  #   this.tail = null;
  #   this._length = 0;
  # }
  # Append value to appropiate spot in list (sorted).
  add(value) {
    var node = ListNode(value);
    if (this.head == null) {
      this.head = node;
      this.tail = node;
    } else {
      var current = this.head;
      var previous = null;
      while (current != null && current.value < value) {
        previous = current;
        current = current.next;
      }
      if (previous == null) {
        node.next = this.head;
        this.head.previous = node;
        this.head = node;
      } else if (current == null) {
        previous.next = node;
        node.previous = previous;
        this.tail = node;
      } else {
        previous.next = node;
        node.previous = previous;
        node.next = current;
        current.previous = node;
      }
    }
    # this._length = this._length + 1;
    this._length += 1;
    return this;
  }

  # add to the beginning of the list
  prepend(value){
    var node = ListNode(value);
    if (this.head == null) {
      this.head = node;
      this.tail = node;
    } else {
      node.next = this.head;
      this.head.previous = node;
      this.head = node;
    }
    # this._length = this._length + 1;
    this._length += 1;
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

  popFirst(){
    if (this.head == null) {
      return;
    }
    var node = this.head;
    var next = node.next;
    this.head = next;
    if (next != null) {
      next.previous = null;
    } else {
      this.tail = null;
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

}

# ~ 
# Helper functions for working with lists.
# ~
class Arrays{
  mergeSort(arr){
    if(len(arr) <= 1){
        return arr;
    }
    var mid = len(arr) // 2;
    var left =arr[:mid]# arr[:len(arr) // 2];    
    var right = arr[mid:]#len(arr) // 2:];
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
  ranged(start, end, step){
    if (step == null){
      step = 1;
    }
    var ret = ArrayList();
    if (step == 0){
      print_error("Error: step cannot be 0");
      return ret;
    }

    if (start > end){
      for (var i = start; i > end; i-= Math.abs(step)){
        ret.add(i);
      }
    }else{
      for (var i = start; i < end; i+= Math.abs(step)){
        ret.add(i);
      }
    }
    return ret;
  }
  fill(size, value){
    var ret = ArrayList();
    for (var i = 0; i < size; i+=1){
      ret.add(value);
    }
    return ret;
  }
  removeIf(arr, f){
    return ArrayList.of(arr).removeAll(f).toArray();
  }
  max(arr){
    if (len(arr) == 0){
      return null;
    }
    var ret = arr[0];
    for (var i = 1; i < len(arr); i+=1){
      if (arr[i] > ret){
        ret = arr[i];
      }
    }
    return ret;
  }
  min(arr){
    if (len(arr) == 0){
      return null;
    }
    var ret = arr[0];
    for (var i = 1; i < len(arr); i+=1){
      if (arr[i] < ret){
        ret = arr[i];
      }
    }
    return ret;
  
  }
  contains(arr, x){
    for (var i = 0; i < len(arr); i+=1){
      if (arr[i] == x){
        return true;
      }
    }
    return false;
  }
}

fn reduce(arr, reduceFn, initialVal){
  var ret = initialVal;
  foreach(arr, fn (x){
    ret = reduceFn(ret, x);
  });
  return ret;
}
fn sum(arr){
  var ret = 0;
  for (var i = 0; i < len(arr); i+=1){
    ret += arr[i];
  }
  return ret;
}
fn foreach_iter(iterable, forFunc){
  print("Warning -- foreach_iter(iterable, forFunc) is deprecated. Use foreach(Collection<Iterable>, forFunc) instead")
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
      return ret;
    } 
  }
}
fn foreach(arr, f){
  for (var i = 0; i < len(arr); i+=1){
    var ret = f(arr[i]);
    if (ret != null && ret){
      return ret;
    }
  }
}
fn enumerate(arr, f){
  for (var i = 0; i < len(arr); i+=1){
    var ret = f(i, arr[i]);
    if (ret != null && ret){
      return ret;
    }
  }
}
fn foreachRevese(arr, f){
  for (var i = len(arr) - 1; i >= 0; i-=1){
    var ret = f(arr[i]);
    if (ret != null && ret){
      return ret;
    }
  }
}
fn map(arr, f){
  var ret = [len(arr)]
  for (var i = 0; i < len(arr); i+=1){
    ret[i] = f(arr[i]);
  }
  return ret;
}

class Math{
  init(){print_error("Cannot instantiate abstract class Math")}
  PI = 3.14;  
  e = 2.71828;
  INF = 1.0/0.0;
  NEG_INF = -1.0/0.0;
  floor(num){
    if (num < 0){
      return num // 1 - 1;
    }
    return num // 1;
  }
  ceil(num){
    if (num < 0 || num == num // 1){
      return num // 1;
    }
    return num//1 + 1;
  }
  sqrt(num){
    return num >= 0 ? num ** 0.5 : 0/0
  }
  gcd(a, b){
    if (a == 0){
      return b;
    }
    return this.gcd(b % a, a);
  }
  lcm(a, b){
    return (a * b) // this.gcd(a, b);
  }
  lcmAll(arr){
    var ret = 1;
    for (var i = 0; i < len(arr); i+=1){
      ret = this.lcm(ret, arr[i]);
    }
    return ret;
  }
  abs(num){
    return num < 0 ? -num : num;
  }
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

## A map implemented as a hash table with separate chaining.

class MapEntry{
  init(key, value){
    this.key = key;
    this.value = value;
  }
  toStr(){
    return f"{this.key}: {this.value}";
  }
  _lt_(other){
    return this.value < other.value
  }
  _gt_(other){
    return this.value > other.value
  }
  _eq_(other){
    if(type(other) != "object" || !instanceof(other, MapEntry)){
      return false;
    }
    return this.value == other.value && this.key == other.key
  }
}

class Map {
  INITIAL_CAPACITY = 64;
  GROW_FACTOR = 2;
  MAX_LOAD_FACTOR = 0.75;
  init(){
    this.capacity = Map.INITIAL_CAPACITY;
    this.entrySet = ArrayList();
    this.entries = [this.capacity];
    this._size = 0;
    this.resizes = 0;
    this.get = this._get_;
    this.put = this._set_;
  }
  _get_(key){
    var hashR = hash(key) % this.capacity;
    var entryList = this.entries[hashR];
    if (null == entryList){
      return null;
    }
    var ret = entryList.findFirst(fn (other) => key == other.key);
    
    if (ret != null){
      return ret.value;
    }
    
    return null;
  }
  _set_(key, value){
    var hashR = hash(key) % this.capacity;
    var entryList = this.entries[hashR];
    if(entryList == null){
      var entry = LinkedList();
      entry.append(MapEntry(key, value));
      this.entries[hashR] = entry;
    }else{
      var node = entryList.head;
      while(node != null){
        if (node.value.key == key){
          var i = this.entrySet.indexOf(MapEntry(key, node.value.value))
          this.entrySet.set(i, MapEntry(key, value));
          node.value.value = value;
          return value;
        }
        node = node.next;
      }
      entryList.append(MapEntry(key, value));
    }
    # this._size = this._size + 1; 
    this._size += 1; 
    this.entrySet.add(MapEntry(key, value));

    if (((this._size) / this.capacity) >= Map.MAX_LOAD_FACTOR){
      this.resize();
    }
    return value;
  }
  getOrDefault(key, defaultValue){
    var ret = this[key];
    if (ret == null){
      return  defaultValue;
    }
    return ret;
  }
  compute(key, computeFn){
    var ret = this[key];
    
    ret = computeFn(key, ret);
    this[key] = ret;
    
    return ret;
  }
  computeIfAbsent(key, computeFn){
    var ret = this[key];
    if (ret == null){
      ret = computeFn(key);
      this[key] = ret;
    }
    return ret;
  }
  computeIfPresent(key, computeFn){
    var ret = this[key];
    if (ret != null){
      ret = computeFn(key, ret);
      this[key] = ret;
    }
    return ret;
  }
  containsKey(key){
    return bool(this.entrySet.foreach(fn (entry){
      if (entry.key == key){
        return true;
      }
    }));
  }
  sortByValue(order){
    if(order == SortOrder.ASC)
      this.entrySet.sort();
    else if(order == SortOrder.DESC)
      this.entrySet.sortDesc();
    return this;
  }
  remove(key){
    var hashRes = hash(key) % this.capacity;
    var entryList = this.entries[hashRes];
    if(entryList == null){
      return null;
    }
    var keyFn = fn (other) => key == other.key
    var ret = entryList.removeIf(keyFn);
    if (ret){
      this._size -= 1;
      this.entrySet.removeIf(keyFn);
      return ret.value;
    }
    return null
  }
  resize(){
    this.resizes += 1;
    var oldValues = this.entries;
    var oldAllValues = this.entrySet;
    this.capacity *= Map.GROW_FACTOR;
    this.entries = [this.capacity];
    this.entrySet = ArrayList();
    this._size = 0;

    var map = this;
    oldAllValues.foreach(fn (entry){
      map[entry.key] = entry.value
    });
  }
  of(entryList){
    if (len(entryList)%2 != 0){
        print_error("Cannot create map from odd number of key value pairs");
        return null;
    }
    var keys = entryList[:@2];
    var values = entryList[1:@2];
    var ret = this.ofSize(max(len(entryList) * 2, Map.INITIAL_CAPACITY));
    for (var i = 0; i < len(keys); i+=1){
      ret[keys[i]] = values[i]
    }
    return ret;
  }
  ofSize(s){
    var ret = Map();
    ret.capacity = max(s*2, Map.INITIAL_CAPACITY);
    ret.entries = [ret.capacity];
    return ret;
  }
  toStr(){
    var ret = StringBuilder("{");
    if(this._size > 0){
      var s = ret;
      this.entrySet.foreach( fn (entry){
        s.append(entry.toStr()).append(", ")
      });
      ret.pop();
    }
    ret.append("}");  
    return ret.toStr();    
  }
  _size_(){
    return this._size;
  }
  isEmpty(){
    return this._size == 0;
  }
  parseFromStr(s){
    var ret = Map();
    var string = trim(s);
    if (string[0] != "{"){
      print_error("Error: parseFromStr(string) requires a string that starts with a {");
      return null;
    }
    if (string[-1] != "}"){
      print_error("Error: parseFromStr(string) requires a string that ends with a }");
      return null;
    }
    string = string[1:-2];
    var arr = string.split(",");
    for (var i = 0; i < len(arr); i+=1){
      var entry = arr[i].split( ":");
      if (len(entry) != 2){
        print_error("Error: parseFromStr(string) requires a string that contains key value pairs separated by :");
        return null;
      }
      var key = entry[0].trim();
      var value = entry[1].trim();
      var keySlice = 1:-2@1
      if (key[0] != "\"" || key[-1] != "\""){
        keySlice = 0:-1@1
      }
      key = key[keySlice];
      ret.put(key, value);
    }
    return ret;
  }
}

class Set <Collection> {
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
        var addToSet = fn (x, s) => fn(x) {s.add(x)}
        foreach(value, addToSet(this));
      }
    }

    if(this._size >= this.capacity * Set.MAX_LOAD_FACTOR)
      this.resize();
    
    var hashRes = hash(value) % this.capacity;
    var entryList = this.entries[hashRes];
    if(entryList == null){
      var entry = [];
      entry.append(value);
      this.entries[hashRes] = entry;
    }else if (entryList.contains(value))
      return value;
    else
      entryList.append(value);

    this.allEntries[this._size] = value;
    this._size = this._size + 1;
    return value;
  }

  _get_(i){
    if (i >= this._size || i < 0){
      print_error("Index out of bounds");
      return null;
    }
    return this.allEntries[i];
  }

  sort(){
    var res = this.allEntries[:this._size].sort();
    # Arrays.mergeSort(res);
    for(var i = 0; i < len(res); i+=1){
      this.allEntries[i] = res[i];
    }
    return res;
  }

  sortDesc(){
    var res = this.allEntries[:this._size];
    Arrays.mergeSort(res);
    var l = len(res)
    for(var i = 0; i < l; i+=1){
      this.allEntries[i] = res[l-i-1];
    }
    return res;
  }

  _size_(){
    return this._size;
  }

  resize(){

    var oldAllValues = this.allEntries;
    var oldSize = this._size;
    this.allEntries = this.allEntries + [this.capacity];
    this.capacity = this.capacity * Set.GROW_FACTOR;
    this.entries = [this.capacity];
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
    var hashRes = hash(value) % this.capacity;
    var entryList = this.entries[hashRes];
    if(null == entryList){
      return false;
    }
    return entryList.contains(value);
  }

  remove(value){
    var hashRes = hash(value) % this.capacity;
    var entryList = this.entries[hashRes];
    if(entryList == null){
      return null;
    }
    var ret = entryList.filter(fn (other) => value == other);
    if (ret != null){
      var temp = Arrays.removeIf(
          this.allEntries[:this._size], 
          fn (other) => value == other
        );
      for (var i = 0; i < len(temp); i+=1){
        this.allEntries[i] = temp[i];
      }
      this._size -= 1
      this.allEntries[this._size] = null
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

class ArrayList <Collection> {
  INITIAL_CAPACITY = 32;
  init(){
    this.capacity = ArrayList.INITIAL_CAPACITY;
    this._size = 0;
    this.entries = [this.capacity]; 
    this.set = this._set_
    this.get = this._get_
  }
  _get_(i){
    if (type(i) == "slice"){
      return this.entries[:this._size][i];
    }
    if(type(i) != "int"){
      print_error("Error: ArrayList.get(i) requires an integer as its argument but the following type was passed in: " + type(i));
      return null;
    }
    if (i >= this._size || i < -this._size){
      print_error("Index out of bounds. Index: " + i + " Size: " + this._size);
      return null;
    }else if (i < 0 ){
      return this.entries[this._size + i];
    }
    return this.entries[i];
  }
  _set_(i, value){
    if (i >= this._size){
      print_error("Index out of bounds");
      return null;
    } else if(i < 0){
      this.entries[this._size + i] = value;
      return value;
    }

    this.entries[i] = value;
    return value;
  }
  getLast(){
    if (this._size == 0){
      # print_error("Cannot get last element of empty list");
      return null;
    }
    return this.entries[this._size - 1];
  }

  set(i, value){
    return this._set_(i, value);
  }

  add(value){
    if (this._size >= this.capacity){
      this.resize();
    }
    this.entries[this._size] = value;
    this._size += 1;
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

  findFirst(f){
    for (var i = 0; i < this._size; i+=1){
      if (f(this.entries[i])){
        return this.entries[i];
      }
    }
    return null;
  }

  indexOf(value){
    for (var i = 0; i < this._size; i+=1){
      if (this.entries[i] == value){
        return i;
      }
    }
    return -1;
  }

  findAll(f){
    var ret = ArrayList();
    for (var i = 0; i < this._size; i+=1){
      if (f(this.entries[i])){
        ret.add(this.entries[i]);
      }
    }
    return ret;
  }

  removeIf(f){
    var i = 0;
    while (i < this._size){
      if (f(this.entries[i])){
        return this.remove(i);
      } 
      i += 1; 
    }
    # return this;
  }

  removeAll(f){
    var i = 0;
    while (i < this._size){
      if (f(this.entries[i])){
        this.remove(i);
      } else{
        i += 1; 
      }
    }
    return this;
  }

  remove(i){
    if (i >= this._size){
      print_error("Index out of bounds, cannot remove element at index " + i + " from list of size " + this._size);
      return null;
    }
    var ret = this.entries[i];
    for (var j = i; j < this._size - 1; j+=1){
      this.entries[j] = this.entries[j+1];
    }
    this.entries[this._size - 1] = null;
    this._size -= 1;
    return ret;
  }

  removeElem(e){
    if (this._size == 0){
      return null;
    }
    var i = 0;
    while (i < this._size){
      if (this.entries[i] == e){
        return this.remove(i);
      } 
      i += 1; 
    }
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

  _size_(){
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

  sortDesc(){
    var res = this.entries[:this._size];
    Arrays.mergeSort(res);
    var l = len(res)
    for(var i = 0; i < l; i+=1){
      this.entries[i] = res[l-i-1];
    }
    return this;
  }

  toStr(){
    return str(this.entries[:this._size]);
  }

  contains(obj){
    for (var i = 0; i < this._size; i+=1){
      if (this.entries[i] == obj){
        return true;
      }
    }
    return false;
  }

  reverse(){
    var temp = [this._size];
    for (var i = 0; i < this._size; i+=1){
      temp[i] = this.entries[this._size - i - 1];
    }
    for (var i = 0; i < this._size; i+=1){
      this.entries[i] = temp[i];
    }
    return this;
  }

  counts(){
    var ret = Map();
    for (var i = 0; i < this._size; i+=1){
      var val = this.entries[i];
      ret.compute(val, fn(k, v) => v == null ? 1 : v + 1);
    }
    return ret;
  }

  clone(){
    return ArrayList.of(this)
  }
}

class Strings{

  WHITE_SPACE = Set.of(" \n\t\r");

  startsWith(string, substr){
    if (len(substr) > len(string)){
      return false;
    }
    for (var i = 0; i < len(substr); i+=1){
      if (string[i] != substr[i]){
        return false;
      }
    }
    return true;
  }

  endsWith(string, substr){
    if (len(substr) > len(string)){
      return false;
    }
    for (var i = 0; i < len(substr); i+=1){
      if (string[len(string) - len(substr) + i] != substr[i]){
        return false;
      }
    }
    return true;
  }

  isWhitespace(char){
    return Strings.WHITE_SPACE.contains(char);
  }

  isDigit(char){
    return char >= "0" && char <= "9";
  }

  isAlpha(char){
    return (char >= "a" && char <= "z") || (char >= "A" && char <= "Z");
  }
}

class Pair {
    init(first, second){
        this.first = first;
        this.second = second;
    }

    _lt_(other){
        return this.wieght < other.wieght;
    }

    _gt_(other){
        return this.wieght > other.wieght;
    }

    toStr(){
        return "(${}, ${})".f(this.first, this.second);
    }
    _eq_(other){
        return this.first == other.first && this.second == other.second;
    }
    _hash_(){
        return hash(this.first) + hash(this.second);
    }
}

class GraphPair<Pair>{
  _lt_(other){
    return this.second < other.second;
  }
  _gt_(other){
    return this.second > other.second;
  }
}

class Graph {
    init(){
        this.adjList = HashMap();
    }

    addEdge(u, v){
        this.adjList.compute(u, fn (k, val) => val ? val.append(v) : {v});
    }

    _get_(u){
        return this.adjList[u];
    }

    _set_(u, v){
        this.addEdge(u, v);
    }

    _hash_(){
        return hash(this.adjList);
    }

    hasEdge(u, v){
        return this.adjList.get(u, []).contains(v);
    }

    minDist(dist, sptSet){
        var min = Math.INF;
        var minIndex = -1;
        dist.keys().foreach(fn (i) {
            if (!sptSet.contains(i) && (dist[i]) < min){
                min = dist[i];
                minIndex = i;
            }
        });
        return minIndex;
    }

    dijkstra(src){
        var dist = HashMap();
        this.adjList.keys().foreach(fn (k) => dist[k] = Math.INF);
        dist[src] = 0;
        # print("dist: ${}".f(dist));
        var sptSet = Set();
        for (var i = 0; i < len(this.adjList); i+=1){
            var u = this.minDist(dist, sptSet);
            # print("u: ${}".f(u));
            sptSet.add(u);
            
            var adj = this.adjList.get(u, []);
            print "adj: ${}".f(adj);
            adj.foreach(fn (v) {
                # print "v: ${}".f(v);
                if (!sptSet.contains(v) && dist[u] != Math.INF && (dist[u] + 1) < dist.get(v, Math.INF)){
                    dist[v] = dist[u] + 1;
                }
            });
        }
        return dist;

    }

    toStr(){
        var s = StringBuilder();
        this.adjList.entries().foreach(fn (e) {
            s.append(e.key).append(" -> ").append(e.value).append("\n");
        });
        return s.toStr();
        # return this.adjList.toStr();
    }
}

class UndirectedGraph <Graph> {
    init(){
        this.adjList = HashMap();
    }

    addEdge(u, v){
        this.adjList.compute(u, fn (k, val) => val ? val.append(v) : {v});
        this.adjList.compute(v, fn (k, val) => val ? val.append(u) : {u});
    }

    hasEdge(u, v){
        return this.adjList.get(u, []).contains(v);
    }

    minDist(dist, sptSet){
        var min = Math.INF;
        var minIndex = -1;
        var d = dist.keys();
        for(var s = 0; s < len(d); s += 1){
            var i = d[s];
            if (!sptSet.contains(i) && (dist[i]) < min){
                min = dist[i];
                minIndex = i;
            }
        }
        return minIndex;
    }

    dijkstra(src){
        var dist = HashMap();
        this.adjList.keys().foreach(fn (k) => dist[k] = Math.INF);
        dist[src] = 0;
        # print("dist: ${}".f(dist));
        # print "adjList: ${}".f(this.adjList);
        var sptSet = [];
        for (var i = 0; i < len(this.adjList.keys()); i+=1){
            var u = this.minDist(dist, sptSet);
            # print("u: ${}".f(u));
            sptSet.append(u);
            
            var adj = this.adjList.get(u, []);
            # print "adj: ${}".f(adj);

            
            adj.foreach(fn (v) {
                var d = dist[u];
                # print "v: ${}".f(v);
                # print "dist[u]: ${}".f(d);
                # print "dist[v]: ${}".f(dist.get(v, Math.INF));
                # print "spSet: ${}".f(sptSet);
                if (!sptSet.contains(v) && d != Math.INF && (d + 1) < dist.get(v, Math.INF)){
                    dist[v] = (dist[u] + 1);
                }
            });
        }
        return dist;

    }

    shortestPathLen(src, dest){
        var dist = this.dijkstra(src);
        return dist[dest];
    }

    shortestPath(src, dest){
        var dist = this.dijkstra(src);
      if (dist.get(dest, Math.INF) == Math.INF){
        return [];
      }
        var ret = [];
        var u = dest;
      var limit = len(this.adjList.keys()) + 1;
      var steps = 0;
        while ( u != src){
            ret.append(u);
            var adj = this.adjList.get(u, []);
        var du = dist.get(u, Math.INF);
        var min = Math.INF;
        var minV = null;
            adj.foreach(fn (v) {
          var dv = dist.get(v, Math.INF);
          if (dv != Math.INF && (dv + 1) == du && dv < min){
            min = dv;
                    minV = v;
                }
            });
        if (minV == null){
          return [];
        }
            u = minV;
        steps += 1;
        if (steps > limit){
          return [];
        }
        }
        ret.append(src);
        return ret[:@-1];

    }

    toStr(){
        var s = StringBuilder();
        
        this.adjList.entries().foreach(fn (e) {
            s.append(e.key).append(" -> ").append(e.value).append("\n");
        });

        return s.toStr();
        # return this.adjList.toStr();
    }

}

class WeightedDirectedGraph <Graph> {
    init(){
        this.adjList = HashMap();
    }

    addEdge(u, v, w){
        this.adjList.compute(u, fn (k, val) => val ? val.append(GraphPair(v, w)) : {GraphPair(v, w)});
    }

    minDist(dist, sptSet){
        var min = Math.INF;
        var minIndex = -1;
        var d = dist.keys();
        for(var s = 0; s < len(d); s += 1){
            var i = d[s];
            if (!sptSet.contains(i) && (dist[i]) < min){
                min = dist[i];
                minIndex = i;
            }
        }
        return minIndex;
    }

    dijkstra(src){
        var dist = HashMap();
        this.adjList.keys().foreach(fn (k) => dist[k] = Math.INF);
        dist[src] = 0;
        # print("dist: ${}".f(dist));
        # print "adjList: ${}".f(this.adjList);
        var sptSet = [];
        for (var i = 0; i < len(this.adjList.keys()); i+=1){
            var u = this.minDist(dist, sptSet);
            # print("u: ${}".f(u));
            sptSet.append(u);
            
            var adj = this.adjList.get(u, []);
            # print "adj: ${}".f(adj);

            
            adj.foreach(fn (vP) {
                var v = vP.first;
                var d = dist[u];
                # print "v: ${}".f(v);
                # print "dist[u]: ${}".f(d);
                # print "dist[v]: ${}".f(dist.get(v, Math.INF));
                # print "spSet: ${}".f(sptSet);
                if (!sptSet.contains(v) && d != Math.INF && (d + 1) < dist.get(v, Math.INF)){
                    dist[v] = (dist[u] + 1);
                }
            });
        }
        return dist;

    }

    shortestPathLen(src, dest){
        var dist = this.dijkstra(src);
        return dist[dest];
    }

    shortestPath(src, dest){
        var dist = this.dijkstra(src);
      if (dist.get(dest, Math.INF) == Math.INF){
        return [];
      }
        var ret = [];
        var u = dest;
      var limit = len(this.adjList.keys()) + 1;
      var steps = 0;
        while (u != src){
            ret.append(u);
        var du = dist.get(u, Math.INF);
            var min = Math.INF;
        var minV = null;
        this.adjList.entries().foreach(fn (e) {
              var srcNode = e.key;
              var dFrom = dist.get(srcNode, Math.INF);
          e.value.foreach(fn (vP) {
            var v = vP.first;
            if (v == u && dFrom != Math.INF && (dFrom + 1) == du && dFrom < min){
              min = dFrom;
                  minV = srcNode;
            }
          });
            });
        if (minV == null){
          return [];
        }
            u = minV;
        steps += 1;
        if (steps > limit){
          return [];
        }
        }
        ret.append(src);
        return ret[:@-1];
        
    }

    toStr(){
        # var s = StringBuilder();
        # s.append("{\n");
        
        # this.adjList.entries().foreach(fn (e) {
        #     s.append(f"   {e.key} -> {e.value}\n");
        # });

        # return s.toStr();
        return this.adjList.toStr();
    }

}


var STDIN = 0
var STDOUT = 1
var STDERR = 2
