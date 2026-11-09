// writer32mx web: talk to writer32mxcdc.c from a browser tab.
//
// The writer is a plain USB CDC device (04d8:000a): bytes sent to it go to
// the target's UART at 115200 bps, the target's UART comes back, an Intel HEX
// stream programs the target over ICSP, and ":FF" resets the target. So the
// page only has to open the port, send text and show what comes back.
//
// Two ways to reach it:
//   Web Serial (desktop Chrome/Edge): the OS CDC driver stays in charge.
//   WebUSB (Chrome on Android): claims the CDC data interface directly.
//
// Two modes:
//   standalone: open index.html (no token): connect, pick a .hex, write it,
//     reset, send text, watch the log.
//   relay: opened as .../#t=<token> from the server (server.py + w32ctl):
//     the page also polls for jobs (run / flash) and uploads the log and
//     events, so whoever runs w32ctl (e.g. Claude) can drive the board.
(function () {
	"use strict";
	var VID = 0x04D8, PID = 0x000A, EP_OUT = 2, EP_IN = 2;

	// --- token: from the fragment (never sent to the server), kept per tab ---
	var T = null;
	(function () {
		var m = /[#&]t=([A-Za-z0-9_-]+)/.exec(location.hash);
		try {
			if (m) sessionStorage.setItem("w32.t", m[1]);
			T = m ? m[1] : sessionStorage.getItem("w32.t");
		} catch (e) { T = m ? m[1] : null; }
		if (m) history.replaceState(null, "", location.pathname);
	})();

	var msg = document.getElementById("msg");
	var app = document.getElementById("app");
	app.innerHTML =
		// One block of controls, at most 26rem wide; the log below takes the width.
		'<div class="blk"><div class="row after"><span id="usb" class="grow"></span>' +
		// Two ways in, stacked: Web Serial (desktop) and WebUSB (Android).
		'<span class="stack"><button id="conn">Connect writer</button>' +
		'<button id="connusb" hidden>Connect via WebUSB</button></span></div>' +
		// The manual controls are hidden in relay mode: the server drives the board.
		'<div id="manual"' + (T ? ' hidden' : '') + '>' +
		'<div class="row"><button id="pickfile">Choose file</button>' +
		'<input type="file" id="file" hidden accept=".hex,.ihx,.txt">' +
		'<span id="chosen" class="grow">No file chosen</span>' +
		// The sample list: the phone's own picker, opened from the ▼ button.
		'<span class="pick" id="samplerow" hidden><button tabindex="-1" aria-hidden="true">&#9660;</button>' +
		'<select id="sample" aria-label="Samples"><option value="">Choose a sample…</option></select></span></div>' +
		'<div class="row end after"><button id="reset" class="alt" disabled>Reset (:FF)</button>' +
		'<button id="write" class="pri" disabled>Write</button></div>' +
		'<div class="row"><input type="text" id="tx" placeholder="Text to send to the target">' +
		'<button id="send" disabled>Send</button></div></div></div>' +
		'<pre id="log"></pre>' +
		'<div class="row end"><button id="clear">Clear</button><button id="save">Save</button></div>' +
		(T ? '<p id="tap" style="font-size:.9em">Tap anywhere on the page once to get vibration alerts (when finished, etc.).</p>' : '') +
		'<pre id="ev"></pre>';
	function $(id) { return document.getElementById(id); }
	var conn = $("conn"), connUsb = $("connusb"), usbP = $("usb"), logP = $("log"), evP = $("ev");
	var btnWrite = $("write"), btnReset = $("reset"), btnSend = $("send");
	// The standalone greeting, with the link to the README (wiring, notes).
	function standaloneMsg() {
		msg.innerHTML = 'Use writer32mxcdc from the browser (Chrome / Edge). ' +
			'<a href="https://github.com/paijp/writer32mx#readme" target="_blank" rel="noopener">Before you use</a>';
	}
	if (T) msg.textContent = "Connecting to the server…"; else standaloneMsg();
	if (T) document.addEventListener("pointerdown", function () {
		var t = $("tap");
		if (t) t.textContent = "Vibration alerts are on.";
	}, { once: true });

	function api(path, body) {
		var o = { headers: { "X-Token": T }, cache: "no-store" };
		if (body !== undefined) { o.method = "POST"; o.body = body; }
		return fetch("api/" + path, o);
	}
	function report(ev) {
		evP.textContent = (new Date().toLocaleTimeString() + " " + JSON.stringify(ev) + "\n" + evP.textContent).slice(0, 4000);
		if (T) return api("event", JSON.stringify(ev)).catch(function () {});
		return Promise.resolve();
	}
	var sleep = function (ms) { return new Promise(function (r) { setTimeout(r, ms); }); };
	var enc = new TextEncoder();

	// --- the log: shown, kept for saving, and (relay mode) uploaded ---
	var logAll = [], logSize = 0, outq = [], flushing = null, dec = new TextDecoder();
	var scan = "";      // recent received text, for waitText()
	function show(text) {
		var stick = logP.scrollTop + logP.clientHeight >= logP.scrollHeight - 4;
		logP.textContent = (logP.textContent + text).slice(-8000);
		if (stick) logP.scrollTop = logP.scrollHeight;
	}
	function received(b) {
		logAll.push(b); logSize += b.length;
		while (logSize > 4 << 20) logSize -= logAll.shift().length;
		if (T) outq.push(b);
		var s = dec.decode(b, { stream: true });
		scan = (scan + s).slice(-8192);
		show(s);
	}
	function mark(text) {
		var b = enc.encode("\n=== " + text + " ===\n");
		logAll.push(b); logSize += b.length;
		if (T) outq.push(b);
		show("\n=== " + text + " ===\n");
	}
	// Serial bytes go up in batches, not one POST per read. Nothing in here
	// may throw: it is also called from the error path of a job.
	function flush() {
		if (!T || !outq.length || flushing) return flushing || Promise.resolve();
		var n = 0; outq.forEach(function (c) { n += c.length; });
		var buf = new Uint8Array(n), o = 0;
		outq.forEach(function (c) { buf.set(c, o); o += c.length; });
		outq = [];
		flushing = api("log", buf).catch(function () {}).then(function () { flushing = null; });
		return flushing;
	}
	if (T) setInterval(flush, 700);
	// Resolve once re matches text received after this call (or fail on bad).
	function waitText(re, ms, bad) {
		scan = "";
		var end = Date.now() + ms;
		return new Promise(function (ok, ng) {
			(function check() {
				if (bad && bad.test(scan)) return ng(new Error("writer: " + bad.exec(scan)[0].trim()));
				if (re.test(scan)) return ok();
				if (!port) return ng(new Error("disconnected"));
				if (Date.now() > end) return ng(new Error("timeout waiting for " + re));
				setTimeout(check, 100);
			})();
		});
	}

	// --- the port: Web Serial or WebUSB behind one small interface ---
	var port = null;        // { kind, write(u8): Promise, close() }
	function connected(p, how) {
		port = p;
		usbP.textContent = "Connected (" + p.kind + ", " + how + ")";
		conn.textContent = "Connected"; conn.disabled = true; connUsb.hidden = true;
		btnWrite.disabled = btnReset.disabled = btnSend.disabled = false;
		report({ what: "claimed", kind: p.kind, how: how });
		if (T) poll();
	}
	function lost(why) {
		if (!port) return;
		port = null;
		usbP.textContent = "Writer disconnected" + (why ? " (" + why + ")" : "") + ".";
		conn.textContent = "Connect writer"; conn.disabled = false;
		connUsb.hidden = !(navigator.serial && navigator.usb);
		btnWrite.disabled = btnReset.disabled = btnSend.disabled = true;
		report({ what: "disconnected", why: why || "" });
	}

	async function openSerial(sp, how) {
		usbP.textContent = "Connecting… (Web Serial, " + how + ")";
		await sp.open({ baudRate: 115200, bufferSize: 65536 });
		try { await sp.setSignals({ dataTerminalReady: true, requestToSend: true }); } catch (e) {}
		var reader = sp.readable.getReader(), writer = sp.writable.getWriter(), closing = false;
		var p = {
			kind: "Web Serial", sp: sp,
			write: function (b) { return writer.write(b); },
			close: async function () {
				closing = true;
				try { await reader.cancel(); } catch (e) {}
				try { writer.releaseLock(); } catch (e) {}
				try { await sp.close(); } catch (e) {}
			}
		};
		connected(p, how);
		(async function () {
			try {
				for (;;) {
					var r = await reader.read();
					if (r.done) break;
					if (r.value && r.value.length) received(r.value);
				}
			} catch (e) { if (!closing) lost(String(e)); return; }
			if (!closing) lost("closed");
		})();
	}

	async function openUsb(d, how) {
		usbP.textContent = "Connecting… (WebUSB, " + how + ")";
		await d.open();
		if (!d.configuration) await d.selectConfiguration(1);
		// The data interface is the one with class 0x0A (interface 1 here).
		var ifn = null, ctl = 0;
		d.configuration.interfaces.forEach(function (i) {
			if (i.alternate.interfaceClass === 0x0A && ifn === null) ifn = i.interfaceNumber;
			if (i.alternate.interfaceClass === 0x02) ctl = i.interfaceNumber;
		});
		if (ifn === null) throw new Error("CDC data interface not found");
		// Android 16 refuses the data interface unless the control
		// interface is claimed first; elsewhere this is harmless.
		try { await d.claimInterface(ctl); } catch (e) {}
		await d.claimInterface(ifn);
		// SET_CONTROL_LINE_STATE (DTR|RTS); the writer only acknowledges it.
		try {
			await d.controlTransferOut({ requestType: "class", recipient: "interface",
				request: 0x22, value: 3, index: ctl });
		} catch (e) {}
		var closing = false;
		var p = {
			kind: "WebUSB",
			write: async function (b) {
				for (var o = 0; o < b.length; o += 4096) await d.transferOut(EP_OUT, b.slice(o, o + 4096));
			},
			close: async function () { closing = true; try { await d.close(); } catch (e) {} }
		};
		p.dev = d;
		connected(p, how);
		(async function () {
			try {
				for (;;) {
					var r = await d.transferIn(EP_IN, 4096);
					if (r.data && r.data.byteLength)
						received(new Uint8Array(r.data.buffer, r.data.byteOffset, r.data.byteLength).slice());
				}
			} catch (e) { if (!closing) lost(String(e)); }
		})();
	}

	function fail(e, what) {
		usbP.textContent = "Cannot connect: " + e;
		report({ what: what || "open-failed", error: String(e) });
	}
	var FILTER = [{ usbVendorId: VID, usbProductId: PID }];
	conn.onclick = function () {
		if (navigator.serial) {
			navigator.serial.requestPort({ filters: FILTER })
				.then(function (sp) { return openSerial(sp, "picked"); })
				.catch(function (e) { fail(e, "pick-failed"); });
		} else connUsb.onclick();
	};
	connUsb.onclick = function () {
		navigator.usb.requestDevice({ filters: [{ vendorId: VID, productId: PID }] })
			.then(function (d) { return openUsb(d, "picked"); })
			.catch(function (e) { fail(e, "pick-failed"); });
	};

	// --- what the writer understands ---
	function resetTarget() { return port.write(enc.encode(":FF\r\n")); }
	function checkHex(text) {
		var lines = text.split(/\r?\n/).filter(function (l) { return l.trim(); }), recs = 0, bytes = 0, eof = false;
		lines.forEach(function (l, i) {
			l = l.trim();
			if (!/^:([0-9A-Fa-f]{2})+$/.test(l)) throw new Error("not Intel HEX at line " + (i + 1));
			var b = l.slice(1).match(/../g).map(function (x) { return parseInt(x, 16); });
			if (b.length !== b[0] + 5) throw new Error("bad record length at line " + (i + 1));
			var sum = 0; b.forEach(function (x) { sum += x; });
			if (sum & 255) throw new Error("checksum mismatch at line " + (i + 1));
			if (b[3] === 0) bytes += b[0];
			if (b[3] === 1) eof = true;
			recs++;
		});
		// The writer programs the last part, the boot flash and the
		// configuration words only on the end-of-file record.
		if (!eof) throw new Error("no end-of-file record (:00000001FF)");
		return { records: recs, bytes: bytes };
	}
	// Send the HEX and wait for the writer's "writing ... run" (the target is
	// released from reset when it prints "run").
	async function writeHex(text, name) {
		var info = checkHex(text);
		report({ what: "image", name: name, records: info.records, bytes: info.bytes });
		if (!T) mark("write " + name + " " + new Date().toISOString());
		var done = waitText(/writing( otf)?\r?\n[\s\S]*\nrun\r?\n/, 300000, /IDCODE invalid\r?\n|FCBUSY stuck[^\n]*\n/);
		done.catch(function () {});
		var b = enc.encode(text.replace(/\r?\n/g, "\n")), t = Date.now();
		for (var o = 0; o < b.length; o += 4096) {
			await port.write(b.slice(o, o + 4096));
			usbP.textContent = "Sending " + Math.round(Math.min(b.length, o + 4096) * 100 / b.length) + "%";
		}
		usbP.textContent = "Waiting for the writer to finish…";
		await done;
		report({ what: "written", ms: Date.now() - t });
	}

	// --- standalone controls ---
	var busy = false;
	async function manual(f) {
		if (busy || !port) return;
		busy = true; btnWrite.disabled = btnReset.disabled = true;
		try { await f(); } catch (e) { usbP.textContent = "Failed: " + e; report({ what: "manual-failed", error: String(e) }); }
		busy = false; if (port) btnWrite.disabled = btnReset.disabled = false;
	}
	btnWrite.onclick = function () {
		manual(async function () {
			var img = await pickedImage();
			if (!img) { usbP.textContent = "Choose a .hex file or a sample (\u25bc) first."; return; }
			await writeHex(img.text, img.name);
			usbP.textContent = "Written: " + img.name;
		});
	};
	btnReset.onclick = function () { manual(function () { return resetTarget(); }); };
	btnSend.onclick = function () {
		if (!port) return;
		var s = $("tx").value + "\n";
		// ':' at the start of a line would be taken for a HEX record.
		if (/(^|\n):/.test(s)) { usbP.textContent = "Not sent: a line starting with ':' would be taken for a HEX record."; return; }
		port.write(enc.encode(s)).catch(function (e) { usbP.textContent = "Send failed: " + e; });
		$("tx").value = "";
	};
	$("tx").addEventListener("keydown", function (e) { if (e.key === "Enter") btnSend.onclick(); });
	$("clear").onclick = function () { logP.textContent = ""; };
	// One choice at a time: a file or a sample, named next to the buttons.
	async function pickedImage() {
		var f = $("file").files[0];
		if (f) return { name: f.name, text: await f.text() };
		var s = $("sample").value;
		if (s) {
			var r = await fetch("samples/" + encodeURIComponent(s), { cache: "no-store" });
			if (!r.ok) throw new Error("could not fetch the sample " + s);
			return { name: s, text: await r.text() };
		}
		return null;
	}
	$("pickfile").onclick = function () { $("file").click(); };
	$("sample").onchange = function () {
		if (!$("sample").value) return;
		$("file").value = "";
		$("chosen").textContent = $("sample").value + " (sample)";
		$("chosen").title = $("sample").selectedOptions[0].textContent;
	};
	$("file").onchange = function () {
		var f = $("file").files[0];
		if (!f) return;
		$("sample").value = "";
		$("chosen").textContent = f.name; $("chosen").title = "";
	};
	// Samples: a list next to the page (samples/index.json), when hosted with one.
	function loadSamples() {
		fetch("samples/index.json", { cache: "no-store" }).then(function (r) { return r.ok ? r.json() : null; })
		.then(function (list) {
			if (!list || !list.length) return;
			list.forEach(function (s) {
				var o = document.createElement("option");
				o.value = s.file; o.textContent = s.title || s.file;
				$("sample").appendChild(o);
			});
			$("samplerow").hidden = false;
		}).catch(function () {});
	}
	if (!T) loadSamples();
	$("save").onclick = function () {
		var a = document.createElement("a");
		a.href = URL.createObjectURL(new Blob(logAll, { type: "application/octet-stream" }));
		a.download = "writer32mx-" + new Date().toISOString().replace(/[:.]/g, "-") + ".log";
		a.click();
		setTimeout(function () { URL.revokeObjectURL(a.href); }, 10000);
	};

	// --- relay mode: messages and jobs from the server ---
	var closed = false, lastMsg = null, lastJob = null, startRev = null, busySince = 0, firstSeq = null, jobsRun = 0;
	function poll() {
		if (!T || closed) return;
		api("poll").then(function (r) { return r.ok ? r.json() : null; }).then(function (c) {
			if (!c) {
				// The token is unknown or expired - typically one this tab kept
				// from an earlier session (it is remembered per tab). Forget it
				// and carry on as the standalone page.
				closed = true;
				try { sessionStorage.removeItem("w32.t"); } catch (e) {}
				T = null; outq = [];
				$("manual").hidden = false;
				var tp = $("tap"); if (tp) tp.remove();
				msg.style.cssText = ""; document.title = "writer32mx";
				standaloneMsg();
				loadSamples();
				return;
			}
			var done = c.state !== "open";
			if (firstSeq === null) firstSeq = c.seq || 0;
			if (c.reloadAsk && !done) {
				if (firstSeq < c.reloadAsk.seq) { c.message = c.reloadAsk.text; c.attention = true; }
				else {
					var tapped = navigator.userActivation && navigator.userActivation.hasBeenActive;
					c.message = tapped ? "Reloaded. Waiting for the next step."
						: "Reloaded. Tap the page once (to turn vibration alerts back on).";
					c.attention = false;
				}
			}
			var key = (done ? "#" : c.attention ? "!" : "") + (c.message || "");
			if (key !== lastMsg) {
				lastMsg = key;
				msg.textContent = c.message || "";
				var box = "color:#000;padding:12px;border-radius:6px;font-weight:bold;background:";
				msg.style.cssText = done ? box + "#81c784" : c.attention ? box + "#ffd54f" : "";
				document.title = (done ? "✓ " : c.attention ? "● " : "") + "writer32mx";
				if ((done || c.attention) && navigator.vibrate) {
					var ok = navigator.vibrate(done ? [400] : [200, 100, 200]);
					report({ what: "vibrate", ok: ok, kind: done ? "close" : "wait",
						activated: !!(navigator.userActivation && navigator.userActivation.hasBeenActive) });
				}
			}
			// A job present when the page loads is not run, so a reload does
			// not repeat the last one; "rev" makes open pages reload.
			if (startRev === null) { startRev = c.rev || 0; lastJob = c.job ? c.job.id : null; }
			else if ((c.rev || 0) !== startRev && (!busy || Date.now() - busySince > 120000)) { location.reload(); return; }
			if (c.job && c.job.id !== lastJob && port && !busy) {
				lastJob = c.job.id;
				runJob(c.job);
			}
			if (done) {
				closed = true;
				conn.disabled = true;
				if (port) port.close();
			}
		}).catch(function () {});
	}

	// job: { id, kind: "reset" | "flash", file, seconds, tx, txDelayMs }
	// flash needs no :FF afterwards: the writer releases reset when done.
	async function runJob(job) {
		busy = true; busySince = Date.now();
		btnWrite.disabled = btnReset.disabled = true;
		if (jobsRun++ === 0 && navigator.vibrate) {
			var vok = navigator.vibrate([80]);
			report({ what: "vibrate", ok: vok, kind: "start",
				activated: !!(navigator.userActivation && navigator.userActivation.hasBeenActive) });
		}
		var t0 = Date.now(), total0 = logSize;
		try {
			report({ what: "job-start", id: job.id, kind: job.kind });
			// The marker goes first: the target starts talking as soon as the
			// writer prints "run", before any await here returns.
			mark("job " + job.id + " " + job.kind + " " + new Date().toISOString());
			total0 = logSize;
			if (job.kind === "flash") {
				var r = await api("file/" + encodeURIComponent(job.file));
				if (!r.ok) throw new Error("could not fetch " + job.file);
				usbP.textContent = "Writing " + job.file + "…";
				await writeHex(await r.text(), job.file);
			} else if (job.kind === "reset" || job.kind === "run") {
				usbP.textContent = "Resetting the target…";
				var ran = waitText(/\nrun\r?\n/, 5000);
				ran.catch(function () {});
				await resetTarget();
				await ran;
			} else throw new Error("unknown job kind " + job.kind);
			usbP.textContent = "Running: reading the log (" + (job.seconds || 10) + " s)";
			var runStart = Date.now(), until = runStart + (job.seconds || 10) * 1000;
			if (job.tx) {
				await sleep(job.txDelayMs === undefined ? 1000 : job.txDelayMs);
				var tx = enc.encode(job.tx);
				mark("tx " + tx.length + " bytes");
				report({ what: "tx", bytes: tx.length });
				await port.write(tx);
			}
			while (Date.now() < until && port) await sleep(100);
			await flush();
			report({ what: "job-done", id: job.id, bytes: logSize - total0, ms: Date.now() - t0 });
			usbP.textContent = "Done: " + (logSize - total0) + " bytes received";
		} catch (e) {
			await flush();
			report({ what: "job-failed", id: job.id, error: String(e), ms: Date.now() - t0 });
			usbP.textContent = "Failed: " + e;
		}
		busy = false;
		if (port) btnWrite.disabled = btnReset.disabled = false;
	}
	if (T) {
		setInterval(function () { if (!document.hidden) poll(); }, 1500);
		setInterval(function () { if (document.hidden && !busy) poll(); }, 5000);
		document.addEventListener("visibilitychange", function () { if (!document.hidden) poll(); });
		poll();
	}

	// --- start: reconnect to a port permitted earlier, without a prompt ---
	// The user agent string is reduced ("Android 10; K"), so ask for the
	// real platform version and model where the browser offers them.
	(navigator.userAgentData && navigator.userAgentData.getHighEntropyValues
		? navigator.userAgentData.getHighEntropyValues(["platform", "platformVersion", "model"]).catch(function () { return {}; })
		: Promise.resolve({})).then(function (h) {
		report({ what: "loaded", ua: navigator.userAgent, os: h.platform, osVersion: h.platformVersion, model: h.model,
			serial: !!navigator.serial, webusb: !!navigator.usb });
	});
	if (!navigator.serial && !navigator.usb) {
		usbP.textContent = "This browser has neither Web Serial nor WebUSB (use Chrome or Edge).";
		conn.disabled = true;
		return;
	}
	connUsb.hidden = !(navigator.serial && navigator.usb);
	var again = navigator.serial
		? navigator.serial.getPorts().then(function (ps) {
			var sp = ps.filter(function (p) { var i = p.getInfo(); return i.usbVendorId === VID && i.usbProductId === PID; })[0];
			if (sp) return openSerial(sp, "remembered").then(function () { return true; });
		})
		: Promise.resolve();
	again.then(function (done) {
		if (done || !navigator.usb) return done;
		return navigator.usb.getDevices().then(function (ds) {
			var d = ds.filter(function (x) { return x.vendorId === VID && x.productId === PID; })[0];
			if (d) return openUsb(d, "remembered").then(function () { return true; });
		});
	}).then(function (done) {
		if (!done) usbP.textContent = "Press Connect writer and choose writer32mx (cdc0008).";
	}).catch(function (e) { fail(e); });
	if (navigator.serial) navigator.serial.addEventListener("disconnect", function (e) { if (port && port.sp === e.target) lost("unplugged"); });
	if (navigator.usb) navigator.usb.addEventListener("disconnect", function (e) { if (port && port.dev === e.device) lost("unplugged"); });
})();
