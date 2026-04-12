# Test Thread pool stats and queue limit controls

from modules.threading import poolStats, setPoolQueueLimit;

var before = poolStats()
print(before.workerCount > 0)
print(before.queueMax >= 1)

print(setPoolQueueLimit(32) == 32)

var after = poolStats()
print(after.queueMax == 32)
print(after.submitted >= 0)
print(after.activeWorkers >= 0)
print(type(after.shutdown) == "bool")

# EXPECTED
# true
# true
# true
# true
# true
# true
# true
# END_EXPECTED
