
class ListNode {
  init(value) {
    this.value = value;
    this.next = null;
    this.previous = null;
  }
}

class List {

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

  # Return the length of the list. The length is tracked as the list
  # is appended to, so this is an O(1) operation.
  length() {
    return this._length;
  }

  # Return the last element, or null if list if empty.
  last() {
    if (this.tail != null) {
      return this.tail.value;
    }
    return null;
  }
}