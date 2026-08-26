from flask import Flask, request, jsonify, render_template_string
from datetime import datetime

app = Flask(__name__)

EXPECTED_API_KEY = "f4d0cb00-dbf5-11f0-bd35-dd4e9bf51317"
latest_payloads = []  # Stores recent incoming telemetry

HTML_TEMPLATE = """
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>ESP32-C6 Position & Telemetry Monitor</title>
    <style>
        body { font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif; background: #0f172a; color: #f8fafc; margin: 0; padding: 20px; }
        h1 { color: #38bdf8; margin-bottom: 5px; }
        .subtitle { color: #94a3b8; font-size: 0.9em; margin-bottom: 20px; }
        table { width: 100%; border-collapse: collapse; background: #1e293b; border-radius: 8px; overflow: hidden; box-shadow: 0 4px 6px -1px rgba(0, 0, 0, 0.3); }
        th, td { padding: 12px 16px; text-align: left; border-bottom: 1px solid #334155; }
        th { background: #334155; color: #f1f5f9; font-weight: 600; }
        tr:hover { background: #243347; }
        .badge { padding: 4px 8px; border-radius: 4px; font-size: 0.85em; font-weight: bold; }
        .conf-high { background: #166534; color: #4ade80; }
        .conf-low { background: #854d0e; color: #facc15; }
        .accel { font-family: monospace; color: #a7f3d0; }
    </style>
    <script>
        // Auto-refresh the page every 2 seconds to fetch fresh telemetry
        setInterval(() => {
            fetch('/api/latest')
                .then(res => res.json())
                .then(data => updateTable(data));
        }, 2000);

        function updateTable(logs) {
            const tbody = document.getElementById('log-body');
            tbody.innerHTML = '';
            logs.forEach(log => {
                const tr = document.createElement('tr');
                tr.innerHTML = `
                    <td>${log.time}</td>
                    <td><strong>${log.device_id}</strong></td>
                    <td>X: ${log.x.toFixed(2)}, Y: ${log.y.toFixed(2)}</td>
                    <td><span class="badge ${log.conf === 'high' ? 'conf-high' : 'conf-low'}">${log.conf}</span></td>
                    <td>${log.b1} (${log.b1_rssi} dBm)</td>
                    <td>${log.n}</td>
                    <td class="accel">X:${log.accel.x.toFixed(2)} Y:${log.accel.y.toFixed(2)} Z:${log.accel.z.toFixed(2)}</td>
                `;
                tbody.appendChild(tr);
            });
        }
    </script>
</head>
<body>
    <h1>ESP32-C6 Local Telemetry Monitor</h1>
    <div class="subtitle">Listening on port 5000 | Auto-refreshing</div>

    <table>
        <thead>
            <tr>
                <th>Time</th>
                <th>Device</th>
                <th>Position (X, Y)</th>
                <th>Confidence</th>
                <th>Strongest Beacon</th>
                <th>Beacons Seen</th>
                <th>Accelerometer (G)</th>
            </tr>
        </thead>
        <tbody id="log-body">
            {% for log in logs %}
            <tr>
                <td>{{ log.time }}</td>
                <td><strong>{{ log.device_id }}</strong></td>
                <td>X: {{ log.x }}, Y: {{ log.y }}</td>
                <td><span class="badge {{ 'conf-high' if log.conf == 'high' else 'conf-low' }}">{{ log.conf }}</span></td>
                <td>{{ log.b1 }} ({{ log.b1_rssi }} dBm)</td>
                <td>{{ log.n }}</td>
                <td class="accel">X:{{ log.accel.x }} Y:{{ log.accel.y }} Z:{{ log.accel.z }}</td>
            </tr>
            {% endfor %}
        </tbody>
    </table>
</body>
</html>
"""

@app.route('/', methods=['GET'])
def index():
    return render_template_string(HTML_TEMPLATE, logs=latest_payloads)

@app.route('/api/latest', methods=['GET'])
def get_latest():
    return jsonify(latest_payloads)

@app.route('/api/push', methods=['POST'])
def handle_push():
    # Validate API key from query parameter or header
    api_key = request.args.get('api_key') or request.headers.get('X-API-Key')
    if api_key != EXPECTED_API_KEY:
        return jsonify({"status": "error", "message": "Unauthorized"}), 401

    data = request.get_json()
    if not data:
        return jsonify({"status": "error", "message": "Invalid JSON"}), 400

    # Add timestamp and append to the list (keeping last 25 records)
    data['time'] = datetime.now().strftime("%H:%M:%S")
    latest_payloads.insert(0, data)
    if len(latest_payloads) > 25:
        latest_payloads.pop()

    print(f"[{data['time']}] Received data from {data.get('device_id')}: Position ({data.get('x')}, {data.get('y')})")
    return jsonify({"status": "success"}), 200

if __name__ == '__main__':
    # Bind to 0.0.0.0 so the ESP32 on the local Wi-Fi network can reach it
    app.run(host='0.0.0.0', port=5000, debug=True)
