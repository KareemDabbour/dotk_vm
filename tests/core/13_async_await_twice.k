async fn worker() {
  return 7
}

var t = worker()
print(await t)
print(await t)

# EXPECTED
# 7
# 7
# END_EXPECTED