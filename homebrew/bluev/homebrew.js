// SPDX-License-Identifier: GPL-3.0-or-later
// BlueV as an entry of the ps5-payload-websrv launcher.
//
// The launcher runs this file in a sandboxed frame on the console, so the
// payload's own web server is reached on the loopback address; it answers
// with CORS headers for that. The tile starts the payload when it is not
// running and, once it is, opens a control menu made of launcher items:
// state, devices (tap to connect), scan, disconnect, stop.

const API = "http://127.0.0.1:7733";

// null when the payload does not answer
async function call(method, path) {
    try {
        const response = await fetch(API + path, { method: method });
        return await response.json();
    } catch (e) {
        return null;
    }
}

function stateLabel(state) {
    return ({
        idle: "Idle",
        scanning: "Scanning",
        connecting: "Connecting",
        streaming: "Streaming"
    })[state] || state;
}

function deviceLine(device) {
    const parts = [device.addr];
    if (device.connected) parts.push("connected");
    if (device.bonded) parts.push("paired");
    if (device.other) parts.push("may not be an audio device");
    if (device.rssi > -128) parts.push(device.rssi + " dBm");
    return parts.join(" · ");
}

// the control menu; every action shows it again with fresh data
async function showPanel() {
    const status = await call("GET", "/api/status");
    if (!status) {
        alert("BlueV is not running. Open its tile to start it.");
        return;
    }
    const devices = (await call("GET", "/api/devices")) || [];
    const busy = status.state !== "idle";
    const radioOn = !!(status.radio && status.radio.on);
    const items = [];

    let detail = status.message || "";
    if (status.state === "streaming") {
        detail = (status.peer.name || status.peer.addr) + " · " +
                 status.stream.rate + " Hz · " +
                 status.stream.pps + " packets/s";
    }
    items.push({
        mainText: stateLabel(status.state),
        secondaryText: detail,
        onclick: async () => { await showPanel(); }       // tap to refresh
    });

    // nothing touches the USB side until the radio is started
    if (!radioOn) {
        items.push({
            mainText: "Start the internal Bluetooth",
            secondaryText: "switch the gamepad on first and leave it alone while it runs",
            onclick: async () => {
                await call("POST", "/api/radio/on?adapter=internal");
                await showPanel();
            }
        });
    } else {
        items.push({
            mainText: "Turn the radio off",
            secondaryText: (status.radio.adapter === "external" ? "external" : "internal") + " adapter",
            onclick: async () => {
                await call("POST", "/api/radio/off");
                await showPanel();
            }
        });
    }

    for (const device of devices) {
        const item = {
            mainText: device.name || device.addr,
            secondaryText: deviceLine(device),
            onclick: async () => {
                await call("POST", "/api/connect?addr=" + encodeURIComponent(device.addr));
                await showPanel();
            }
        };
        if (device.bonded) {
            item.options = [{
                text: "Forget this pairing",
                onclick: async () => {
                    await call("POST", "/api/forget?addr=" + encodeURIComponent(device.addr));
                    await showPanel();
                }
            }];
        }
        items.push(item);
    }

    items.push({
        mainText: "Scan for devices",
        secondaryText: !radioOn ? "start the radio first"
                       : busy ? "not while " + status.state : "30 seconds",
        onclick: async () => {
            await call("POST", "/api/scan");
            await showPanel();
        }
    });
    items.push({
        mainText: "Disconnect",
        secondaryText: "end the stream",
        onclick: async () => {
            await call("POST", "/api/disconnect");
            await showPanel();
        }
    });
    items.push({
        mainText: "Stop BlueV",
        secondaryText: "releases the Bluetooth controller to the system",
        onclick: async () => {
            await call("POST", "/api/quit");
            return false;
        }
    });
    showCarousel(items);
}

async function main() {
    const WORKDIR = window.workingDir;

    return {
        mainText: "BlueV",
        secondaryText: "Bluetooth audio",
        // running: open the menu; not running: let the launcher start it
        onclick: async () => {
            if (await call("GET", "/api/status")) {
                await showPanel();
                return false;
            }
            return { path: WORKDIR + "/eboot.elf", cwd: WORKDIR, daemon: true };
        },
        options: [
            { text: "Control menu", onclick: async () => { await showPanel(); } },
            { text: "Stop", onclick: async () => { await call("POST", "/api/quit"); } }
        ]
    };
}
