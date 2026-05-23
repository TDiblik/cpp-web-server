local header_content = string.rep("B", 65000)

wrk.method = "GET"
wrk.headers["X-Large-Header"] = header_content
