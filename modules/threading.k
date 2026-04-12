module threading;

fn awaitTimeout(thread, timeoutMs) {
    if (thread == null) {
        print_error("awaitTimeout requires a Thread instance");
    }
    return thread.join(timeoutMs);
}

fn currentThread() {
    return Thread.currentThread();
}

fn checkCancelled() {
    var thread = currentThread();
    if (thread == null) {
        return false;
    }
    return thread.checkCancelled();
}

fn poolStats() {
    return Thread.poolStats();
}

fn setPoolQueueLimit(maxPending) {
    return Thread.setPoolQueueLimit(maxPending);
}

export awaitTimeout, currentThread, checkCancelled, poolStats, setPoolQueueLimit;
