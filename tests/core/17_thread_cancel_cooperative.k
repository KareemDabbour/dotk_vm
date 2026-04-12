# Test cooperative cancellation with Thread.cancel/isCancelled/checkCancelled

from modules.threading import awaitTimeout, checkCancelled;

fn worker(limit) {
    var i = 0
    while (i < limit) {
        if (checkCancelled()) {
            return "cancelled"
        }
        i = i + 1
    }
    return "done"
}

var t = Thread(worker, 100000000)
print(t.isCancelled())
print(t.cancel())
print(t.isCancelled())
print(awaitTimeout(t, 2) == "cancelled")
print(t.done())

# EXPECTED
# false
# true
# true
# true
# true
# END_EXPECTED
