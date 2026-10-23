// writer32mx USB diagnostics: runs WebUSB / Web Serial steps sent from the
// server (job kind "diag", job.ops = [{op: ..., ...}, ...]) and reports each
// result as an event.  Only a fixed set of operations exists; nothing sent
// from the server is evaluated as code.
(function () {
	"use strict";
	var T = null;
	try {
		var m = location.hash.match(/t=([A-Za-z0-9_-]+)/);
		if (m) { sessionStorage.setItem("w32diag", m[1]); history.replaceState(null, "", location.pathname); }
		T = sessionStorage.getItem("w32diag");
	} catch (e) {}
	var $ = function (id) { return document.getElementById(id); };
	var logEl = $("log"), msgEl = $("msg");
	function api(path, body) {
		var o = { headers: { "X-Token": T || "" } };
		if (body !== undefined) { o.method = "POST"; o.body = JSON.stringify(body); o.headers["Content-Type"] = "application/json"; }
		return fetch("../api/" + path, o);
	}
	function show(s) {
		logEl.textContent += s + "\n";
		if (logEl.textContent.length > 20000) logEl.textContent = logEl.textContent.slice(-15000);
		logEl.scrollTop = logEl.scrollHeight;
	}
	function report(ev) {
		show(JSON.stringify(ev));
		if (T) api("event", ev).catch(function () {});
	}
	function hex(u8) {
		return Array.prototype.map.call(u8, function (x) { return (x < 16 ? "0" : "") + x.toString(16); }).join(" ");
	}
	function unhex(s) {
		return new Uint8Array((s.replace(/[^0-9a-fA-F]/g, "").match(/../g) || []).map(function (x) { return parseInt(x, 16); }));
	}
	function sleep(ms) { return new Promise(function (r) { setTimeout(r, ms); }); }
	function withTimeout(p, ms) {
		return Promise.race([p, sleep(ms).then(function () { throw new Error("timeout " + ms + " ms"); })]);
	}

	var dev = null, sp = null, reader = null;
	function devInfo(d) {
		var r = { vid: d.vendorId, pid: d.productId, product: d.productName, maker: d.manufacturerName,
			serial: d.serialNumber, opened: d.opened, usb: d.usbVersionMajor + "." + d.usbVersionMinor,
			cls: d.deviceClass, configs: [] };
		d.configurations.forEach(function (c) {
			r.configs.push({ value: c.configurationValue, ifs: c.interfaces.map(function (i) {
				return { n: i.interfaceNumber, claimed: i.claimed, alts: i.alternates.map(function (a) {
					return { alt: a.alternateSetting, cls: a.interfaceClass, sub: a.interfaceSubclass,
						proto: a.interfaceProtocol, eps: a.endpoints.map(function (e) {
							return e.direction + e.endpointNumber + "/" + e.type + "/" + e.packetSize; }) };
				}) };
			}) });
		});
		if (d.configuration) r.active = d.configuration.configurationValue;
		return r;
	}
	async function needDev(o) {
		if (o.index !== undefined || !dev) {
			var l = await navigator.usb.getDevices();
			dev = l[o.index || 0] || null;
		}
		if (!dev) throw new Error("no permitted USB device (tap 「USB デバイスを選ぶ」)");
		return dev;
	}

	var ops = {
		info: async function () {
			var h = navigator.userAgentData && navigator.userAgentData.getHighEntropyValues
				? await navigator.userAgentData.getHighEntropyValues(["platform", "platformVersion", "model"]).catch(function () { return {}; }) : {};
			var r = { ua: navigator.userAgent, os: h.platform, osVersion: h.platformVersion, model: h.model, serial: !!navigator.serial, usb: !!navigator.usb, usbDevices: [], serialPorts: [] };
			if (navigator.usb) (await navigator.usb.getDevices()).forEach(function (d) { r.usbDevices.push(devInfo(d)); });
			if (navigator.serial) (await navigator.serial.getPorts()).forEach(function (p) { r.serialPorts.push(p.getInfo()); });
			return r;
		},
		open: async function (o) { var d = await needDev(o); await d.open(); return devInfo(d); },
		close: async function (o) { var d = await needDev(o); await d.close(); return "closed"; },
		reset: async function (o) { var d = await needDev(o); await d.reset(); return "reset"; },
		forget: async function (o) { var d = await needDev(o); await d.forget(); dev = null; return "forgotten"; },
		config: async function (o) { var d = await needDev(o); await d.selectConfiguration(o.value || 1); return devInfo(d); },
		claim: async function (o) { var d = await needDev(o); await d.claimInterface(o.n); return devInfo(d); },
		release: async function (o) { var d = await needDev(o); await d.releaseInterface(o.n); return "released " + o.n; },
		alt: async function (o) { var d = await needDev(o); await d.selectAlternateInterface(o.n, o.alt); return "alt"; },
		ctrlOut: async function (o) {
			var d = await needDev(o);
			var r = await d.controlTransferOut({ requestType: o.requestType || "class", recipient: o.recipient || "interface",
				request: o.request, value: o.value || 0, index: o.idx || 0 }, unhex(o.data || ""));
			return { status: r.status, bytes: r.bytesWritten };
		},
		ctrlIn: async function (o) {
			var d = await needDev(o);
			var r = await d.controlTransferIn({ requestType: o.requestType || "standard", recipient: o.recipient || "device",
				request: o.request, value: o.value || 0, index: o.idx || 0 }, o.len || 64);
			return { status: r.status, data: r.data ? hex(new Uint8Array(r.data.buffer, r.data.byteOffset, r.data.byteLength)) : "" };
		},
		out: async function (o) {
			var d = await needDev(o);
			var data = o.text !== undefined ? new TextEncoder().encode(o.text) : unhex(o.hex || "");
			var r = await withTimeout(d.transferOut(o.ep, data), o.timeoutMs || 3000);
			return { status: r.status, bytes: r.bytesWritten };
		},
		"in": async function (o) {
			var d = await needDev(o), all = [], text = "", until = Date.now() + (o.ms || 2000);
			while (Date.now() < until) {
				try {
					var r = await withTimeout(d.transferIn(o.ep, o.len || 64), until - Date.now() + 10);
				} catch (e) { if (/timeout/.test(String(e))) break; throw e; }
				if (r.data && r.data.byteLength) {
					var u = new Uint8Array(r.data.buffer, r.data.byteOffset, r.data.byteLength);
					all.push(hex(u)); text += new TextDecoder().decode(u);
				}
				if (!o.loop) break;
			}
			return { hex: all.join(" | ").slice(0, 2000), text: text.slice(0, 2000) };
		},
		serialOpen: async function (o) {
			var l = await navigator.serial.getPorts();
			sp = l[o.index || 0];
			if (!sp) throw new Error("no permitted serial port (tap 「シリアルを選ぶ」)");
			await sp.open({ baudRate: o.baud || 115200 });
			return sp.getInfo();
		},
		serialWrite: async function (o) {
			var w = sp.writable.getWriter();
			try { await w.write(o.text !== undefined ? new TextEncoder().encode(o.text) : unhex(o.hex || "")); } finally { w.releaseLock(); }
			return "written";
		},
		serialRead: async function (o) {
			var text = "", until = Date.now() + (o.ms || 2000);
			reader = sp.readable.getReader();
			try {
				while (Date.now() < until) {
					var r;
					try { r = await withTimeout(reader.read(), until - Date.now() + 10); }
					catch (e) { if (/timeout/.test(String(e))) break; throw e; }
					if (r.done) break;
					text += new TextDecoder().decode(r.value);
				}
			} finally {
				try { await reader.cancel(); } catch (e) {}
				reader.releaseLock(); reader = null;
			}
			return { text: text.slice(0, 2000) };
		},
		serialClose: async function () { await sp.close(); return "closed"; },
		wait: async function (o) { await sleep(o.ms || 1000); return "waited"; }
	};

	async function runJob(job) {
		report({ what: "diag-start", id: job.id, n: job.ops.length });
		for (var i = 0; i < job.ops.length; i++) {
			var o = job.ops[i], t0 = Date.now();
			try {
				if (!ops[o.op]) throw new Error("unknown op " + o.op);
				var r = await ops[o.op](o);
				report({ what: "diag", id: job.id, i: i, op: o.op, ok: true, ms: Date.now() - t0, result: r });
			} catch (e) {
				report({ what: "diag", id: job.id, i: i, op: o.op, ok: false, ms: Date.now() - t0, error: String(e) });
				if (!o.cont) break;
			}
		}
		report({ what: "diag-end", id: job.id });
	}

	$("pickUsb").onclick = function () {
		navigator.usb.requestDevice({ filters: [] })
			.then(function (d) { dev = d; report({ what: "picked-usb", dev: devInfo(d) }); })
			.catch(function (e) { report({ what: "pick-usb-failed", error: String(e) }); });
	};
	$("pickSerial").onclick = function () {
		navigator.serial.requestPort({})
			.then(function (p) { report({ what: "picked-serial", info: p.getInfo() }); })
			.catch(function (e) { report({ what: "pick-serial-failed", error: String(e) }); });
	};
	if (!navigator.usb) $("pickUsb").disabled = true;
	if (!navigator.serial) $("pickSerial").disabled = true;
	document.addEventListener("click", function once() {
		document.removeEventListener("click", once);
		report({ what: "tapped" });
	});

	var lastJob = null, started = false, lastMsg = null;
	function poll() {
		if (!T) { msgEl.textContent = "トークンがありません"; return; }
		api("poll").then(function (r) { return r.ok ? r.json() : null; }).then(function (c) {
			if (!c) { msgEl.textContent = "セッションが無効です"; return; }
			if (c.message !== lastMsg) {
				lastMsg = c.message; msgEl.textContent = c.message || "";
				msgEl.className = c.attention ? "hl" : "";
				if (lastMsg && navigator.vibrate) navigator.vibrate([150, 100, 150]);
			}
			if (!started) { started = true; lastJob = c.job ? c.job.id : null; ops.info().then(function (r) { report({ what: "diag-loaded", info: r }); }); return; }
			if (c.job && c.job.kind === "diag" && c.job.id !== lastJob) { lastJob = c.job.id; runJob(c.job); }
		}).catch(function () {});
	}
	setInterval(poll, 1500);
	poll();
})();
