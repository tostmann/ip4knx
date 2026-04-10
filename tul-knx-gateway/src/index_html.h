#pragma once

const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="de">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>TUL KNX/IP Gateway</title>
    <link rel="icon" href="data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 64 64'%3E%3Crect width='64' height='64' rx='12' fill='%2374b836'/%3E%3Ctext x='16' y='46' font-family='Verdana' font-size='40' font-weight='bold' fill='white'%3Eb%3C/text%3E%3C/svg%3E">
    <style>
        :root {
            --primary-color: #74b836; /* Busware Green */
            --bg-color: #f4f7f6;
            --card-bg: #ffffff;
            --text-color: #333;
            --border-radius: 8px;
            --danger: #ef4444;
            --success: #10b981;
        }
        body {
            font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif;
            background-color: var(--bg-color);
            color: var(--text-color);
            margin: 0;
            display: flex;
            flex-direction: column;
            min-height: 100vh;
        }
        .navbar {
            background-color: white;
            color: var(--text-color);
            padding: 1rem 2rem;
            display: flex;
            justify-content: space-between;
            align-items: center;
            box-shadow: 0 2px 4px rgba(0,0,0,0.1);
            border-bottom: 3px solid var(--primary-color);
        }
        .navbar-brand {
            display: flex;
            align-items: center;
            font-size: 1.5rem;
            font-weight: bold;
        }
        .container {
            max-width: 1200px;
            margin: 2rem auto;
            padding: 0 1rem;
            flex-grow: 1;
            width: 90%;
        }
        .grid {
            display: grid;
            grid-template-columns: repeat(auto-fit, minmax(300px, 1fr));
            gap: 1.5rem;
        }
        .card {
            background: var(--card-bg);
            border-radius: var(--border-radius);
            box-shadow: 0 4px 6px rgba(0,0,0,0.05);
            padding: 1.5rem;
            border: 1px solid #e0e0e0;
            display: flex;
            flex-direction: column;
        }
        .card-header {
            border-bottom: 2px solid var(--bg-color);
            margin-bottom: 1rem;
            padding-bottom: 0.5rem;
            font-weight: bold;
            font-size: 1.1rem;
            color: var(--primary-color);
        }
        .card-body {
            flex-grow: 1;
            line-height: 1.6;
        }
        .status-badge {
            display: inline-block;
            padding: 0.25rem 0.75rem;
            border-radius: 20px;
            font-size: 0.85rem;
            font-weight: bold;
        }
        .status-online { background: #d4edda; color: #155724; }
        .status-offline { background: #f8d7da; color: #721c24; }
        footer {
            text-align: center;
            padding: 1rem;
            font-size: 0.9rem;
            color: #666;
            background: #eee;
        }
        .info-row {
            display: flex;
            justify-content: space-between;
            border-bottom: 1px solid #eee;
            padding: 5px 0;
        }
        .info-row span:first-child {
            font-weight: bold;
        }
        /* Modal Styles */
        .modal {
            display: none; position: fixed; z-index: 1000; left: 0; top: 0;
            width: 100%; height: 100%; overflow: auto; background-color: rgba(0,0,0,0.4);
        }
        .modal-content {
            background-color: #fefefe; margin: 10% auto; padding: 20px;
            border: 1px solid #888; width: 90%; max-width: 400px; border-radius: var(--border-radius);
            box-shadow: 0 4px 8px rgba(0,0,0,0.2);
        }
        .close { color: #aaa; float: right; font-size: 28px; font-weight: bold; cursor: pointer; }
        .close:hover, .close:focus { color: black; text-decoration: none; cursor: pointer; }
        .form-group { margin-bottom: 15px; }
        .form-group label { display: block; margin-bottom: 5px; font-weight: bold; }
        .form-group input, .form-group select { width: 100%; padding: 8px; box-sizing: border-box; border: 1px solid #ccc; border-radius: 4px; }
        .btn { background-color: var(--primary-color); color: white; padding: 10px 15px; border: none; border-radius: 4px; cursor: pointer; width: 100%; font-size: 16px; margin-top: 10px; }
        .btn:hover { opacity: 0.9; }
        #scan-btn { background-color: #6c757d; margin-bottom: 15px; }
    </style>
</head>
<body>
    <nav class="navbar">
        <div class="navbar-brand">
            <img src="data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAToAAAAyCAYAAADMZheFAAAABGdBTUEAALGPC/xhBQAAACBjSFJNAAB6JgAAgIQAAPoAAACA6AAAdTAAAOpgAAA6mAAAF3CculE8AAAACXBIWXMAAAsTAAALEwEAmpwYAAAFFWlUWHRYTUw6Y29tLmFkb2JlLnhtcAAAAAAAPHg6eG1wbWV0YSB4bWxuczp4PSJhZG9iZTpuczptZXRhLyIgeDp4bXB0az0iWE1QIENvcmUgNS40LjAiPgogICA8cmRmOlJERiB4bWxuczpyZGY9Imh0dHA6Ly93d3cudzMub3JnLzE5OTkvMDIvMjItcmRmLXN5bnRheC1ucyMiPgogICAgICA8cmRmOkRlc2NyaXB0aW9uIHJkZjphYm91dD0iIgogICAgICAgICAgICB4bWxuczp0aWZmPSJodHRwOi8vbnMuYWRvYmUuY29tL3RpZmYvMS4wLyIKICAgICAgICAgICAgeG1sbnM6ZXhpZj0iaHR0cDovL25zLmFkb2JlLmNvbS9leGlmLzEuMC8iCiAgICAgICAgICAgIHhtbG5zOmRjPSJodHRwOi8vcHVybC5vcmcvZGMvZWxlbWVudHMvMS4xLyIKICAgICAgICAgICAgeG1sbnM6eG1wPSJodHRwOi8vbnMuYWRvYmUuY29tL3hhcC8xLjAvIgogICAgICAgICAgICB4bWxuczp4bXBNTT0iaHR0cDovL25zLmFkb2JlLmNvbS94YXAvMS4wL21tLyI+CiAgICAgICAgIDx0aWZmOlJlc29sdXRpb25Vbml0PjI8L3RpZmY6UmVzb2x1dGlvblVuaXQ+CiAgICAgICAgIDx0aWZmOk9yaWVudGF0aW9uPjE8L3RpZmY6T3JpZW50YXRpb24+CiAgICAgICAgIDxleGlmOlBpeGVsWERpbWVuc2lvbj4yMDg4PC9leGlmOlBpeGVsWERpbWVuc2lvbj4KICAgICAgICAgPGV4aWY6Q29sb3JTcGFjZT4xPC9leGlmOkNvbG9yU3BhY2U+CiAgICAgICAgIDxleGlmOlBpeGVsWURpbWVuc2lvbj4zMzM8L2V4aWY6UGl4ZWxZRGltZW5zaW9uPgogICAgICAgICA8ZGM6Zm9ybWF0PmltYWdlL2pwZWc8L2RjOmZvcm1hdD4KICAg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" style="height:40px; margin-right:15px;" alt="busware.de">
            TUL KNX/IP Gateway
        </div>
        <div class="nav-links">
            <span id="system-status" class="status-badge status-online" style="cursor: pointer;" onclick="openWifiModal()" title="Klicken für WLAN Einstellungen">Verbunden</span>
        </div>
    </nav>

    <main class="container">
        <div class="grid">
            
            <section class="card">
                <div class="card-header">System Status</div>
                <div class="card-body">
                    <div class="info-row"><span>Uptime:</span> <span id="uptime">-</span></div>
                    <div class="info-row"><span>WiFi SSID:</span> <span id="wifi_ssid">-</span></div>
                    <div class="info-row"><span>IP Adresse:</span> <span id="ip_addr">-</span></div>
                    <div class="info-row"><span>MAC Adresse:</span> <span id="mac_addr">-</span></div>
                </div>
            </section>

            <section class="card">
                <div class="card-header">KNX Parameter</div>
                <div class="card-body">
                    <div class="info-row"><span>Programmiert (ETS):</span> <span id="knx_configured">-</span></div>
                    <div class="info-row"><span>Physikalische Adresse:</span> <span id="knx_pa">-</span></div>
                    <div class="info-row"><span>Status LED Pin:</span> <span id="knx_led_pin">-</span></div>
                    <div class="info-row"><span>Prog Button Pin:</span> <span id="knx_btn_pin">-</span></div>
                </div>
            </section>

            <section class="card">
                <div class="card-header">KNXnet/IP Clients</div>
                <div class="card-body">
                    <div class="info-row"><span>Tunneling Slots (Max):</span> <span id="knx_max_tunnels">-</span></div>
                    <div class="info-row"><span>Aktive Clients:</span> <span id="active_clients">-</span></div>
                    <br>
                    <small style="color:#666;">
                        Das Gateway unterst&uuml;tzt parallele KNXnet/IP Tunneling-Verbindungen (z.B. f&uuml;r ETS & HomeAssistant).
                    </small>
                </div>
            </section>

            <section class="card">
                <div class="card-header">KNX Bus Statistik (UART)</div>
                <div class="card-body">
                    <div class="info-row"><span>Buslast:</span> <span id="bus_load">-</span> %</div>
                    <div class="info-row"><span>Empfangene Telegramme (RX):</span> <span id="rx_frames">-</span></div>
                    <div class="info-row"><span>Gesendete Telegramme (TX):</span> <span id="tx_frames">-</span></div>
                    <div class="info-row"><span>Empfangene Bytes (RX):</span> <span id="rx_bytes">-</span></div>
                    <div class="info-row"><span>Gesendete Bytes (TX):</span> <span id="tx_bytes">-</span></div>
                </div>
            </section>

            <section class="card">
                <div class="card-header">Firmware & System</div>
                <div class="card-body">
                    <div class="info-row"><span>Version:</span> <span id="fw_version">-</span></div>
                    <div class="info-row"><span>Build Nummer:</span> <span id="build_number">-</span></div>
                    <div class="info-row"><span>Git Hash:</span> <span id="build_git">-</span></div>
                </div>
            </section>

            <section class="card">
                <div class="card-header">Hardware Info</div>
                <div class="card-body">
                    <div class="info-row"><span>Prozessor:</span> <span id="hw_cpu">-</span></div>
                    <div class="info-row"><span>Taktfrequenz:</span> <span id="hw_freq">-</span> MHz</div>
                    <div class="info-row"><span>RAM (Gesamt):</span> <span id="hw_ram_total">-</span> KB</div>
                    <div class="info-row"><span>RAM (Frei):</span> <span id="hw_ram_free">-</span> KB</div>
                </div>
            </section>

        </div>
    </main>

    <footer>
        TUL/TUL32 KNX/IP Gateway Firmware - basierend auf <a href="https://github.com/OpenKNX" target="_blank" style="color:var(--primary-color);text-decoration:none;">OpenKNX</a> | 
        <a href="https://github.com/tostmann/ip4knx" target="_blank" style="color:var(--primary-color);text-decoration:none;">GitHub Repository</a><br>
        Version: <span id="footer_version">-</span> |
        Build: <span id="footer_build">-</span> (<span id="footer_git">-</span>)
    </footer>

    <!-- WiFi Modal -->
    <div id="wifiModal" class="modal">
        <div class="modal-content">
            <span class="close" onclick="closeWifiModal()">&times;</span>
            <h2 style="margin-top:0; color:var(--primary-color);">WLAN Konfiguration</h2>
            <button id="scan-btn" class="btn" onclick="scanWifi()">WLAN Netzwerke suchen</button>
            <div class="form-group">
                <label for="ssid">Netzwerkname (SSID):</label>
                <select id="ssid-select" style="display:none;" onchange="document.getElementById('ssid').value=this.value;">
                    <option value="">Wählen Sie ein Netzwerk...</option>
                </select>
                <input type="text" id="ssid" placeholder="Ihre SSID eingeben">
            </div>
            <div class="form-group">
                <label for="password">Passwort (PSK):</label>
                <input type="password" id="password" placeholder="Passwort (optional)">
            </div>
            <button class="btn" onclick="connectWifi()">Verbinden & Neustarten</button>
            <hr style="margin: 20px 0; border: 0; border-top: 1px solid #ccc;">
            <button class="btn" style="background-color: #ef4444;" onclick="startApMode()">Als Access Point (AP) neustarten</button>
        </div>
    </div>

    <script>
        function openWifiModal() { document.getElementById('wifiModal').style.display = 'block'; }
        function closeWifiModal() { document.getElementById('wifiModal').style.display = 'none'; }
        
        function scanWifi() {
            let btn = document.getElementById('scan-btn');
            btn.innerText = 'Suche läuft...';
            btn.disabled = true;
            fetch('/api/wifi/scan')
                .then(r => r.json())
                .then(data => {
                    let uniqueNets = {};
                    data.forEach(net => {
                        if (!uniqueNets[net.ssid] || uniqueNets[net.ssid].rssi < net.rssi) {
                            uniqueNets[net.ssid] = net;
                        }
                    });
                    let sortedNets = Object.values(uniqueNets).sort((a, b) => b.rssi - a.rssi);

                    let sel = document.getElementById('ssid-select');
                    sel.innerHTML = '<option value="">Wählen Sie ein Netzwerk...</option>';
                    sortedNets.forEach(net => {
                        if(net.ssid) {
                            let opt = document.createElement('option');
                            opt.value = net.ssid;
                            opt.text = net.ssid + ' (' + net.rssi + ' dBm)';
                            sel.appendChild(opt);
                        }
                    });
                    sel.style.display = 'block';
                    btn.innerText = 'WLAN Netzwerke suchen';
                    btn.disabled = false;
                })
                .catch(e => {
                    alert('Fehler beim Scannen!');
                    btn.innerText = 'WLAN Netzwerke suchen';
                    btn.disabled = false;
                });
        }
        
        function connectWifi() {
            let ssid = document.getElementById('ssid').value;
            let pass = document.getElementById('password').value;
            if(!ssid) { alert('SSID darf nicht leer sein!'); return; }
            
            let formData = new URLSearchParams();
            formData.append('ssid', ssid);
            formData.append('password', pass);
            
            fetch('/api/wifi/connect', { method: 'POST', body: formData })
                .then(r => r.json())
                .then(d => {
                    if(d.status === 'ok') {
                        alert('Konfiguration gespeichert. Das Gateway startet nun neu.');
                        closeWifiModal();
                    } else {
                        alert('Fehler: ' + d.error);
                    }
                }).catch(e => alert('Fehler beim Senden!'));
        }

        function startApMode() {
            if(!confirm('WLAN-Daten löschen und Gateway dauerhaft im AP-Modus neustarten?')) return;
            fetch('/api/wifi/ap_mode', { method: 'POST' })
                .then(r => r.json())
                .then(d => {
                    if(d.status === 'ok') {
                        alert('WLAN-Daten gelöscht. Das Gateway startet nun im AP-Modus neu.');
                        closeWifiModal();
                    } else {
                        alert('Fehler: ' + d.error);
                    }
                }).catch(e => alert('Fehler beim Senden!'));
        }


        function updateStatus() {
            fetch('/api/status')
                .then(response => response.json())
                .then(data => {
                    document.getElementById('uptime').innerText = data.uptime;
                    document.getElementById('wifi_ssid').innerText = data.ssid;
                    document.getElementById('ip_addr').innerText = data.ip;
                    document.getElementById('mac_addr').innerText = data.mac;
                    
                    document.getElementById('knx_configured').innerText = data.knx_configured ? 'Ja' : 'Nein';
                    document.getElementById('knx_pa').innerText = data.knx_pa;
                    document.getElementById('knx_led_pin').innerText = data.knx_led_pin;
                    document.getElementById('knx_btn_pin').innerText = data.knx_btn_pin;
                    document.getElementById('knx_max_tunnels').innerText = data.knx_max_tunnels;
                    
                    document.getElementById('active_clients').innerText = data.active_clients;
                    
                    if (data.rx_frames !== undefined) {
                        document.getElementById('rx_frames').innerText = data.rx_frames;
                        document.getElementById('tx_frames').innerText = data.tx_frames;
                        document.getElementById('rx_bytes').innerText = data.rx_bytes;
                        document.getElementById('tx_bytes').innerText = data.tx_bytes;
                        document.getElementById('bus_load').innerText = data.bus_load;
                    }
                    
                    let badge = document.getElementById('system-status');
                    if (data.is_ap_mode) {
                        badge.innerText = 'AP Modus Aktiv';
                        badge.className = 'status-badge status-online';
                        badge.style.backgroundColor = '#0288d1'; // distinct color for AP mode
                        badge.style.color = '#ffffff';
                    } else if (data.wifi_connected) {
                        badge.innerText = 'WLAN Verbunden';
                        badge.className = 'status-badge status-online';
                        badge.style.backgroundColor = ''; // reset style in case it was set
                        badge.style.color = '';
                    } else {
                        badge.innerText = 'WLAN Getrennt';
                        badge.className = 'status-badge status-offline';
                        badge.style.backgroundColor = ''; // reset style in case it was set
                        badge.style.color = '';
                    }

                    // Build info
                    if (data.build) {
                        document.getElementById('fw_version').innerText = data.build.version;
                        document.getElementById('build_number').innerText = data.build.number;
                        document.getElementById('build_git').innerText = data.build.git;
                        
                        document.getElementById('footer_version').innerText = data.build.version;
                        document.getElementById('footer_build').innerText = data.build.number;
                        document.getElementById('footer_git').innerText = data.build.git;
                    }
                    
                    // Hardware info
                    if (data.hardware) {
                        document.getElementById('hw_cpu').innerText = data.hardware.chip_model + " (Rev " + data.hardware.chip_rev + ")";
                        document.getElementById('hw_freq').innerText = data.hardware.cpu_freq;
                        document.getElementById('hw_ram_total').innerText = Math.round(data.hardware.heap_total / 1024);
                        document.getElementById('hw_ram_free').innerText = Math.round(data.hardware.heap_free / 1024);
                    }
                })
                .catch(error => console.error('Error fetching status:', error));
        }

        updateStatus();
        setInterval(updateStatus, 5000);
    </script>
</body>
</html>
)rawliteral";