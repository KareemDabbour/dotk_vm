var m = .{}
m["k"] = {1, 2, 3, 4}
print(len(m["k"]))
print(gc() >= 0)
print(len(m["k"]))

var append = m["k"].append
gc()
append(5)
print(len(m["k"]))
import "sql"
var db = SQL(":memory:")
var result = db.query("SELECT 'kept' AS value")
gc()
print(result.columns[0])
print(result.rows[0][0])
print(db.close())

# EXPECTED
# 4
# true
# 4
# 5
# value
# kept
# true
# END_EXPECTED
