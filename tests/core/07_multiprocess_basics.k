print(getpid() > 0)
print(getppid() > 0)

var pid = spawn("exit 7")
var r = waitPid(pid)

print(type(r))
print(r.pid == pid)
print(r.exited)
print(r.exitCode == 7)
print(r.signaled == false)

# EXPECTED
# true
# true
# object
# true
# true
# true
# true
# END_EXPECTED
