if (!{}) {
  print("empty-list-falsey")
} else {
  print("bad-list")
}

if (!.{}) {
  print("empty-map-falsey")
} else {
  print("bad-map")
}

if ("") {
  print("bad-empty-str")
} else {
  print("empty-str-falsey")
}

if (0) {
  print("bad-zero")
} else {
  print("zero-falsey")
}

if (2 + 3 > 1) {
  print("const-branch-true")
} else {
  print("bad-const-true")
}

if ((2 - 2) && true) {
  print("bad-const-false")
} else {
  print("const-branch-false")
}

# EXPECTED
# empty-list-falsey
# empty-map-falsey
# empty-str-falsey
# zero-falsey
# const-branch-true
# const-branch-false
# END_EXPECTED
