local body_content = string.rep("A", 10485760)
local header_content = string.rep("B", 65000)

wrk.method = "POST"
wrk.body = body_content
wrk.headers["Content-Type"] = "text/plain"
wrk.headers["X-Large-Header"] = header_content
