module coroutines;

from modules.threading import awaitTimeout;

var _selectCursor = 0;

fn _contentionPause(spins) {
    if (spins < 64) {
        yieldNow();
        return;
    }
    # After enough contention, briefly sleep to reduce hot-spin CPU usage.
    sleepFor(0.0005)
}

fn spawn(taskFn, *args) {
    # Keep this wrapper small; co_spawn supports variadic args and does arity checks.
    return co_spawn(taskFn, *args)
}

fn yieldNow() {
    return co_yield()
}

fn sleepFor(seconds) {
    return co_sleep(seconds)
}

class Channel {
    init(capacity) {
        if (type(capacity) != "int") {
            print_error("Channel(capacity) expects an int")
        }
        if (capacity < 0) {
            print_error("Channel(capacity) expects capacity >= 0")
        }
        this.capacity = capacity;
        this.closed = false;
        this.queue = {};
    }

    close() {
        this.closed = true;
        return true;
    }

    isClosed() {
        return this.closed;
    }

    size() {
        return len(this.queue);
    }

    _canSend() {
        if (this.capacity == 0) {
            return true;
        }
        return len(this.queue) < this.capacity;
    }

    send(value) {
        # var spins = 0;
        while (true) {
            if (this.closed) {
                print_error("send on closed channel")
            }

            if (this._canSend()) {
                this.queue.append(value);
                return true;
            }

            yieldNow();
        #     _contentionPause(spins);
        #     spins = spins + 1;
        }
    }

    sendTimeout(value, timeoutMs) {
        if (type(timeoutMs) != "int") {
            print_error("sendTimeout(value, timeoutMs) expects timeoutMs as int")
        }
        if (timeoutMs < 0) {
            print_error("sendTimeout(value, timeoutMs) expects timeoutMs >= 0")
        }

        var deadline = time() + (timeoutMs / 1000.0);
        # var spins = 0;
        while (true) {
            if (this.closed) {
                print_error("send on closed channel")
            }

            if (this._canSend()) {
                this.queue.append(value);
                return true;
            }

            if (time() >= deadline) {
                return false;
            }
        #     _contentionPause(spins);
        #     spins = spins + 1;
        yieldNow();
        }
    }

    tryRecv() {
        if (len(this.queue) > 0) {
            return this.queue.pop(0);
        }
        return null;
    }

    recv() {
            # var spins = 0;
            while (true) {
            if (len(this.queue) > 0) {
                return this.queue.pop(0);
            }
            if (this.closed) {
                return null;
            }
        #     _contentionPause(spins);
        #     spins = spins + 1;
        yieldNow();
        }
    }

    recvTimeout(timeoutMs) {
        if (type(timeoutMs) != "int") {
            print_error("recvTimeout(timeoutMs) expects timeoutMs as int")
        }
        if (timeoutMs < 0) {
            print_error("recvTimeout(timeoutMs) expects timeoutMs >= 0")
        }

        var deadline = time() + (timeoutMs / 1000.0);
        # var spins = 0;
        while (true) {
            if (len(this.queue) > 0) {
                return this.queue.pop(0);
            }
            if (this.closed) {
                return null;
            }
            if (time() >= deadline) {
                return null;
            }
            # _contentionPause(spins);
            # spins = spins + 1;
        yieldNow();
        }
    }

    toStr(){
        return f"Channel(closed={this.closed}, size={len(this.queue)}, queue={this.queue[:3]}...)"
    }
}

fn channel(capacity) {
    return Channel(capacity);
}

fn recvSelect(channels, timeoutMs) {
    if (type(channels) != "list") {
        print_error("recvSelect(channels, timeoutMs) expects channels as list")
    }
    if (type(timeoutMs) != "int") {
        print_error("recvSelect(channels, timeoutMs) expects timeoutMs as int")
    }
    if (timeoutMs < 0) {
        print_error("recvSelect(channels, timeoutMs) expects timeoutMs >= 0")
    }

    var deadline = time() + (timeoutMs / 1000.0);
    # var spins = 0;
    while (true) {
        var n = len(channels);
        if (n == 0) {
            return  {false, -1, null, false, true};
        }

        var start = _selectCursor % n;
        var offset = 0;
        while (offset < n) {
            var i = (start + offset) % n;
            var ch = channels[i];
            var v = ch.tryRecv();
            if (v != null) {
                # [hasValue, index, value, isClosed, isTimeout]
                _selectCursor = _selectCursor + 1;
                return {true, i, v, false, false};
            }

            if (ch.isClosed()) {
                _selectCursor = _selectCursor + 1;
                return {false, i, null, true, false};
            }
            offset = offset + 1;
        }

        if (time() >= deadline) {
            return  {false, -1, null, false, true};
        }
        # return  {false, -1, null, false, true};
    #     _contentionPause(spins);
    #     spins = spins + 1;
    yieldNow();
    }
}

fn recvSelectAny(channels) {
    while (true) {
        var r = recvSelect(channels, 50);
        if (!r[4]) {
            return r;
        }
    }
}

fn sendSelect(channels, value, timeoutMs) {
    if (type(channels) != "list") {
        print_error("sendSelect(channels, value, timeoutMs) expects channels as list")
    }
    if (type(timeoutMs) != "int") {
        print_error("sendSelect(channels, value, timeoutMs) expects timeoutMs as int")
    }
    if (timeoutMs < 0) {
        print_error("sendSelect(channels, value, timeoutMs) expects timeoutMs >= 0")
    }

    var deadline = time() + (timeoutMs / 1000.0);
    # var spins = 0;
    while (true) {
        var n = len(channels);
        if (n == 0) {
            return {false, -1, false, true};
        }

        var anyOpen = false;
        var start = _selectCursor % n;
        var offset = 0;
        while (offset < n) {
            var i = (start + offset) % n;
            var ch = channels[i];
            if (!ch.isClosed()) {
                anyOpen = true;
                if (ch._canSend()) {
                    ch.send(value)
                    _selectCursor = _selectCursor + 1;
                    # [sent, index, allClosed, isTimeout]
                    return {true, i, false, false};
                }
            }
            offset = offset + 1;
        }

        if (!anyOpen) {
            return {false, -1, true, false};
        }

        if (time() >= deadline) {
            return {false, -1, false, true};
        }
        yieldNow();

    #     _contentionPause(spins);
    #     spins = spins + 1;
    }
}

export spawn, yieldNow, sleepFor, awaitTimeout, Channel, channel, recvSelect, recvSelectAny, sendSelect;
