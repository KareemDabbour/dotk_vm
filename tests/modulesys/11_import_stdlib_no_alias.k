import stdlib;

var xs = stdlib.LinkedList();
xs.append(1);
xs.append(2);

print(xs._size_());
print(xs.last());

# EXPECTED
# 2
# 2
# END_EXPECTED
