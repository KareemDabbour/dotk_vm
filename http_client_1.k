import "http.k"

var client = HttpClient("http://localhost:8080")

var res = client.get("/")
print(f"status={res.status} ok={res.ok}")
print(res.statusText)
print(len(res.body))

# Example POST:
# var p = client.post("/api", "hello from dotk")
# print(p.status)
# print(p.body)
