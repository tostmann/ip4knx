#pragma once

const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="de">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>TUL KNX/IP Gateway</title>
    <link rel="icon" href="data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 64 64'%3E%3Crect width='64' height='64' rx='12' fill='%2300549f'/%3E%3Cpath d='M32 44a4 4 0 1 1 0-8 4 4 0 0 1 0 8zm-10-14a14 14 0 0 1 20 0m-28-10a24 24 0 0 1 36 0' fill='none' stroke='white' stroke-width='5' stroke-linecap='round'/%3E%3C/svg%3E">
    <style>
        :root {
            --primary-color: #00549f; /* Typisches KNX Blau */
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
            background-color: var(--primary-color);
            color: white;
            padding: 1rem 2rem;
            display: flex;
            justify-content: space-between;
            align-items: center;
            box-shadow: 0 2px 4px rgba(0,0,0,0.1);
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
    </style>
</head>
<body>
    <nav class="navbar">
        <div class="navbar-brand">
            <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 64 64" style="height:40px; margin-right:15px;">
                <rect width="64" height="64" rx="12" fill="#fff"/>
                <path d="M32 44a4 4 0 1 1 0-8 4 4 0 0 1 0 8zm-10-14a14 14 0 0 1 20 0m-28-10a24 24 0 0 1 36 0" fill="none" stroke="#00549f" stroke-width="5" stroke-linecap="round"/>
            </svg>
            TUL KNX/IP Gateway
        </div>
        <div class="nav-links">
            <span id="system-status" class="status-badge status-online">Verbunden</span>
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
                </div>
            </section>

        </div>
    </main>

    <footer>
        TUL/TUL32 KNX/IP Gateway Firmware - basierend auf OpenKNX
    </footer>

    <script>
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
                        document.getElementById('bus_load').innerText = data.bus_load;
                    }
                    
                    let badge = document.getElementById('system-status');
                    if (data.wifi_connected) {
                        badge.innerText = 'WLAN Verbunden';
                        badge.className = 'status-badge status-online';
                    } else {
                        badge.innerText = 'WLAN Getrennt';
                        badge.className = 'status-badge status-offline';
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