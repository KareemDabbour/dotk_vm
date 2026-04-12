# Test Thread.currentThread() and thread-bound checkCancelled()

from modules.threading import awaitTimeout, currentThread;

fn worker(limit) {
    var me = currentThread()
    var i = 0
    while (i < limit) {
        if (me.checkCancelled()) {
            return "cancelled"
        }
        i = i + 1
    }
    return "done"
}

var mainThread = currentThread()
print(mainThread != null)
print(mainThread.checkCancelled())

var t = Thread(worker, 100000000)
print(t.cancel())
print(awaitTimeout(t, 1000) == "cancelled")
print(t.isCancelled())
print(t.done())

# EXPECTED
# true
# false
# true
# true
# true
# true
# END_EXPECTED
