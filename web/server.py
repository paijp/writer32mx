#!/usr/bin/env python3
"""writer32mx web relay: a browser tab that flashes and watches a target
through writer32mxcdc, driven from the server side (w32ctl), e.g. by Claude.

The page has no login. Each session is a random token that only the VPS can
mint (w32ctl new), handed to the user as https://<host>/writer32mx/#t=<token>.
The token sits after '#', so it never reaches nginx's access log or a Referer;
the page sends it in a header. The page itself (writer32mx.html, one file with
its script inline) is public and also works on its own without a token; only
the api/ calls need one.

Per session, under DATA/s/<sha256(token)>/:
  control.json  written by w32ctl: state, message, expires, job
  events.jsonl  appended by the page: what it saw and did
  log.bin       appended by the page: raw serial output
  files/        what the page may download (the .hex to write)
"""
import base64, hashlib, json, os, re, time
from http.server import ThreadingHTTPServer, BaseHTTPRequestHandler

DATA = os.environ.get("W32_DATA", "/srv/writer32mx")
PORT = int(os.environ.get("W32_PORT", "5311"))
P = os.environ.get("W32_PREFIX", "/writer32mx")
MAXBODY = 256 * 1024
TOKEN_RE = re.compile(r"^[A-Za-z0-9_-]{20,64}$")

CSP = ("default-src 'none'; script-src 'none'; style-src 'unsafe-inline'; "
       "connect-src 'self'; base-uri 'none'; form-action 'none'; frame-ancestors 'none'")


def page():
    """The page and a CSP that allows exactly its inline script (by hash)."""
    with open(os.path.join(DATA, "app", "writer32mx.html"), "rb") as f:
        html = f.read()
    m = re.search(rb"<script>(.*?)</script>", html, re.S)
    h = base64.b64encode(hashlib.sha256(m.group(1)).digest()).decode()
    return html, CSP.replace("script-src 'none'", "script-src 'sha256-%s'" % h)


def session_dir(token):
    if not token or not TOKEN_RE.match(token):
        return None
    d = os.path.join(DATA, "s", hashlib.sha256(token.encode()).hexdigest())
    try:
        with open(os.path.join(d, "control.json")) as f:
            c = json.load(f)
    except (OSError, ValueError):
        return None
    if time.time() > c.get("expires", 0):
        return None
    return d, c


class H(BaseHTTPRequestHandler):
    server_version = "writer32mx"

    def log_message(self, fmt, *a):
        pass  # no access log: the path never carries the token, but keep it quiet

    def send(self, code, body=b"", ctype="text/plain; charset=utf-8", extra=None, csp=CSP):
        if isinstance(body, str):
            body = body.encode()
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.send_header("Referrer-Policy", "no-referrer")
        self.send_header("X-Content-Type-Options", "nosniff")
        self.send_header("Content-Security-Policy", csp)
        for k, v in (extra or {}).items():
            self.send_header(k, v)
        self.end_headers()
        self.wfile.write(body)

    def path_only(self):
        return self.path.split("?", 1)[0]

    def authed(self, closed_ok=False):
        # A closed session can still be polled until it expires, so the page
        # can show the closing message; nothing else works once it is closed.
        r = session_dir(self.headers.get("X-Token", ""))
        if r and (closed_ok or r[1].get("state") == "open"):
            return r[0]
        self.send(404, "not found")
        return None

    def do_GET(self):
        p = self.path_only()
        if p in (P, P + "/"):
            if p == P:
                return self.send(301, "", extra={"Location": P + "/"})
            html, csp = page()
            return self.send(200, html, "text/html; charset=utf-8", csp=csp)
        if p == P + "/api/poll":
            d = self.authed(closed_ok=True)
            if not d:
                return
            seen = os.path.join(d, "seen")
            if not os.path.exists(seen):
                open(seen, "w").write(str(time.time()))
            with open(os.path.join(d, "control.json"), "rb") as f:
                return self.send(200, f.read(), "application/json")
        m = re.match("^" + re.escape(P) + r"/api/file/([A-Za-z0-9._-]{1,80})$", p)
        if m:
            d = self.authed()
            if not d:
                return
            fp = os.path.join(d, "files", m.group(1))
            if not os.path.isfile(fp):
                return self.send(404, "no such file")
            with open(fp, "rb") as f:
                return self.send(200, f.read(), "application/octet-stream")
        self.send(404, "not found")

    def do_POST(self):
        p = self.path_only()
        if p not in (P + "/api/event", P + "/api/log"):
            return self.send(404, "not found")
        # Events are still accepted from a closed session, so the page can
        # report what it did on closing (the vibration); the log is not.
        d = self.authed(closed_ok=(p == P + "/api/event"))
        if not d:
            return
        n = int(self.headers.get("Content-Length") or 0)
        if n < 0 or n > MAXBODY:
            return self.send(413, "too large")
        body = self.rfile.read(n)
        if p == P + "/api/log":
            with open(os.path.join(d, "log.bin"), "ab") as f:
                f.write(body)
        else:
            try:
                ev = json.loads(body)
            except ValueError:
                return self.send(400, "bad json")
            line = json.dumps({"t": round(time.time(), 3), "ev": ev}, ensure_ascii=False)
            with open(os.path.join(d, "events.jsonl"), "a") as f:
                f.write(line + "\n")
        self.send(200, "ok")


def live_sessions():
    n = 0
    for name in os.listdir(os.path.join(DATA, "s")):
        try:
            with open(os.path.join(DATA, "s", name, "control.json")) as f:
                if time.time() <= json.load(f).get("expires", 0):
                    n += 1
        except (OSError, ValueError):
            pass
    return n


def reaper(srv):
    # The container is started by `w32ctl new` and is not meant to outlive
    # its sessions: once none is left (closed ones linger 10 min to show their
    # message), stop serving and let the container exit.
    while True:
        time.sleep(60)
        if live_sessions() == 0:
            srv.shutdown()
            return


if __name__ == "__main__":
    import threading
    srv = ThreadingHTTPServer(("0.0.0.0", PORT), H)
    threading.Thread(target=reaper, args=(srv,), daemon=True).start()
    srv.serve_forever()
