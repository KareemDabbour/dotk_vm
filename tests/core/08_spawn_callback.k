var pid1 = spawn(fn() {
  return 7
})
var r1 = waitPid(pid1)
print(r1.pid == pid1)
print(r1.exited)
print(r1.exitCode == 7)

var pid2 = spawn(fn(a, b) {
  if (a + b == 5) {
    return 0
  }
  return 1
}, 2, 3)
var r2 = waitPid(pid2)
print(r2.exited)
print(r2.exitCode == 0)

# EXPECTED
# true
# true
# true
# true
# true
# END_EXPECTED
