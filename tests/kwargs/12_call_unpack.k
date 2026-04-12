fn score(a, b, c) {
  return (a * 100) + (b * 10) + c;
}

fn sum(a, b, *rest) {
  var s = a + b;
  var i = 0;
  while (i < len(rest)) {
    s = s + rest[i];
    i = i + 1;
  }
  return s;
}

var args = {2}
var kwargs = .{"c"::9}

print(score(1, *args, **kwargs))
print(sum(1, 2, *{3, 4, 5}))
print(sum(1, 2, *{}))

# EXPECTED
# 129
# 15
# 3
# END_EXPECTED
