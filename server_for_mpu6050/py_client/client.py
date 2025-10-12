import matplotlib.pyplot as plt
from matplotlib.animation import FuncAnimation
import numpy as np
import math, time, threading
import requests, json
from collections import deque

try:
    from sseclient import SSEClient
except Exception:
    SSEClient = None

# ---------- Config ----------
UI_DT = 1.0 / 90.0  # draw interval
SAMPLER_HZ = 10
CORR_GAIN = 1.0  # accel tilt correction gain (0 = none, 0.5..3 typical)

# Gyro bias handling to reduce drift when stationary
STARTUP_CALIBRATION_S = 2.0  # seconds to average gyro at startup (keep sensor still)
AUTO_BIAS = True  # adapt bias slowly when the device is detected stationary
AUTO_BIAS_ALPHA = 0.005  # 0..1 per-sample update rate for bias when stationary
STATIONARY_GYRO_DPS_THRESH = 1.5  # dps threshold to consider "not rotating"
ACCEL_NORM_TARGET = 1.0  # expected |accel| at rest (in g)
ACCEL_NORM_TOL = 0.2  # +/- tolerance around target (g)

# Integrator timing
INTEGRATOR_HZ = 60  # Physics update rate (Hz)

# Temperature plot settings
TEMP_WINDOW_S = 60  # seconds of temperature history to display
MAX_TEMP_POINTS = 600  # cap number of stored points (avoid unbounded growth)

# Data source: 'json' (polling) or 'sse' (server-sent events)
DATA_SOURCE = "sse"  # change to 'sse' for streaming
JSON_URL = "http://localhost:3737/json"  # returns a JSON object
SSE_URL = "http://localhost:3737/events"  # Content-Type: text/event-stream

# Axis remap/sign to match your board. Swap ('y','x','z') if X/Y are interchanged.
ACCEL_AXIS_MAP = ("x", "y", "z")  # was ('x','y','z')
ACCEL_AXIS_SIGN = (1, 1, 1)  # set an entry to -1 to invert that axis
GYRO_AXIS_MAP = ("x", "y", "z")
GYRO_AXIS_SIGN = (1, 1, 1)


# ---------- Math helpers ----------
def _remap_vec(d, amap, asign):
    return np.array([d[amap[i]] * asign[i] for i in range(3)], dtype=float)


def _quat_mul(q, r):
    w1, x1, y1, z1 = q
    w2, x2, y2, z2 = r
    return np.array(
        [
            w1 * w2 - x1 * x2 - y1 * y2 - z1 * z2,
            w1 * x2 + x1 * w2 + y1 * z2 - z1 * y2,
            w1 * y2 - x1 * z2 + y1 * w2 + z1 * x2,
            w1 * z2 + x1 * y2 - y1 * x2 + z1 * w2,
        ],
        dtype=float,
    )


def _quat_norm(q):
    n = np.linalg.norm(q)
    return q / (n if n > 0 else 1.0)


def _quat_to_R(q):
    w, x, y, z = q
    ww, xx, yy, zz = w * w, x * x, y * y, z * z
    return np.array(
        [
            [ww + xx - yy - zz, 2 * (x * y - w * z), 2 * (x * z + w * y)],
            [2 * (x * y + w * z), ww - xx + yy - zz, 2 * (y * z - w * x)],
            [2 * (x * z - w * y), 2 * (y * z + w * x), ww - xx - yy + zz],
        ],
        dtype=float,
    )


# ---------- Orientation state (quaternion body->world) ----------
q = np.array([1.0, 0.0, 0.0, 0.0], dtype=float)
R = np.eye(3)
_running = True
_lock = threading.Lock()

# For HUD
_last_accel = np.array([0.0, 0.0, 1.0])
_last_gyro_dps = np.array([0.0, 0.0, 0.0])
_last_temp_c = float("nan")

# Gyro bias state (in dps)
_g_bias_dps = np.zeros(3)
_calib_sum = np.zeros(3)
_calib_count = 0
_calib_end_time = time.monotonic() + STARTUP_CALIBRATION_S

# Temperature history (timestamp, temp_c)
_temp_history = deque(maxlen=MAX_TEMP_POINTS)


def _parse_payload(obj):
    """Extract accel, gyro, and temperature fields from the payload."""
    a_raw = {"x": obj["ax"], "y": obj["ay"], "z": obj["az"]}
    g_raw = {"x": obj["gx"], "y": obj["gy"], "z": obj["gz"]}
    temp_c = obj["temp"]

    return a_raw, g_raw, temp_c


def _ingest_sample(a_raw, g_raw, temp_c):
    """Convert raw sensor payloads, update bias, and store the latest readings."""
    global _last_accel, _last_gyro_dps, _last_temp_c, _g_bias_dps, _calib_sum, _calib_count

    a_b = _remap_vec(a_raw, ACCEL_AXIS_MAP, ACCEL_AXIS_SIGN)
    g_meas_dps = _remap_vec(g_raw, GYRO_AXIS_MAP, GYRO_AXIS_SIGN)

    now = time.monotonic()
    if now < _calib_end_time:
        _calib_sum += g_meas_dps
        _calib_count += 1
        if _calib_count > 0:
            _g_bias_dps = _calib_sum / max(1, _calib_count)
    elif AUTO_BIAS:
        accel_norm = np.linalg.norm(a_b)
        if (
            np.linalg.norm(g_meas_dps - _g_bias_dps) < STATIONARY_GYRO_DPS_THRESH
            and abs(accel_norm - ACCEL_NORM_TARGET) <= ACCEL_NORM_TOL
        ):
            _g_bias_dps = (
                1 - AUTO_BIAS_ALPHA
            ) * _g_bias_dps + AUTO_BIAS_ALPHA * g_meas_dps

    g_dps = g_meas_dps - _g_bias_dps

    with _lock:
        _last_accel = a_b
        _last_gyro_dps = g_dps
        if temp_c is not None:
            try:
                _last_temp_c = float(temp_c)
            except (TypeError, ValueError):
                pass
            else:
                _temp_history.append((now, _last_temp_c))


def _integrate(dt):
    if dt <= 0:
        return
    global q, R
    with _lock:
        a_b = _last_accel
        g_dps = _last_gyro_dps
        omega = np.radians(g_dps)
        g_w = np.array([0.0, 0.0, 1.0])
        an = a_b / (np.linalg.norm(a_b) + 1e-9)
        v_b = R.T @ g_w
        e = np.cross(v_b, an)
        omega_corr = omega + CORR_GAIN * e
        q_dot = 0.5 * _quat_mul(q, np.array([0.0, *omega_corr]))
        q[:] = _quat_norm(q + q_dot * dt)
        R[:] = _quat_to_R(q)


def _sampler():
    global _running
    if DATA_SOURCE == "json":
        period = 1.0 / SAMPLER_HZ
        session = requests.Session()
        while _running:
            try:
                resp = session.get(JSON_URL, timeout=1.0)
                resp.raise_for_status()
                obj = resp.json()
                a_raw, g_raw, temp_c = _parse_payload(obj)
                _ingest_sample(a_raw, g_raw, temp_c)
            except Exception:
                # brief backoff on error
                time.sleep(min(0.2, period))
            # pace polling
            time.sleep(period)
    elif DATA_SOURCE == "sse":
        if SSEClient is None:
            raise RuntimeError(
                "Install sseclient (or sseclient-py) for SSE: pip install sseclient"
            )
        while _running:
            try:
                # Note: some versions of sseclient do not accept a 'timeout' kwarg.
                # Use defaults here; if you need a timeout, pass a configured requests.Session to SSEClient.
                messages = SSEClient(SSE_URL)
                # https://github.com/btubbs/sseclient/blob/7c94b8333f1fd2ec2e9f3bd809d34534b85830e1/test_sseclient.py#L282
                for _, ev in enumerate(messages, start=1):
                    if not _running:
                        break
                    if not ev.data:
                        continue
                    obj = json.loads(ev.data)
                    a_raw, g_raw, temp_c = _parse_payload(obj)
                    _ingest_sample(a_raw, g_raw, temp_c)
            except Exception as e:
                # Surface the error so it's not silent, then reconnect after short delay
                print(f"[SSE] reconnecting after error: {e!r}")
                time.sleep(0.5)
    else:
        raise ValueError(f"Unknown DATA_SOURCE: {DATA_SOURCE}")


threading.Thread(target=_sampler, daemon=True).start()


def _integrator_thread():
    period = 1.0 / INTEGRATOR_HZ
    while _running:
        _integrate(period)
        time.sleep(period)


threading.Thread(target=_integrator_thread, daemon=True).start()

# ---------- Plot setup ----------
cube = (
    np.array(
        [
            [-1, -1, -1],
            [1, -1, -1],
            [1, 1, -1],
            [-1, 1, -1],
            [-1, -1, 1],
            [1, -1, 1],
            [1, 1, 1],
            [-1, 1, 1],
        ]
    )
    * 0.5
)

edges = [
    [0, 1],
    [1, 2],
    [2, 3],
    [3, 0],
    [4, 5],
    [5, 6],
    [6, 7],
    [7, 4],
    [0, 4],
    [1, 5],
    [2, 6],
    [3, 7],
]

fig = plt.figure(constrained_layout=True, figsize=(13, 6))
fig.suptitle("MPU6050 Live Telemetry", fontsize=14, fontweight="bold")
gs = fig.add_gridspec(2, 2, width_ratios=(2.4, 1), height_ratios=(3, 1))

ax_orient = fig.add_subplot(gs[:, 0], projection="3d")
ax_orient.set_xlim(-1, 1)
ax_orient.set_ylim(-1, 1)
ax_orient.set_zlim(-1, 1)
ax_orient.set_box_aspect((1, 1, 1))
ax_orient.set_xlabel("X", fontsize=11, color="red")
ax_orient.set_ylabel("Y", fontsize=11, color="green")
ax_orient.set_zlabel("Z", fontsize=11, color="blue")
ax_orient.view_init(elev=25, azim=135)
ax_orient.grid(False)

ax_temp = fig.add_subplot(gs[0, 1])
ax_temp.set_xlabel("Elapsed (s)")
ax_temp.set_ylabel("Temperature (°C)")
ax_temp.set_title("Temperature Trend", pad=12)
ax_temp.grid(True, alpha=0.3)
ax_temp.margins(x=0)

ax_info = fig.add_subplot(gs[1, 1])
ax_info.axis("off")
ax_info.set_title("Latest Sample", loc="left", pad=12)

lines = [ax_orient.plot([], [], [], "gray", linewidth=1.5)[0] for _ in edges]
axis_lines = [
    ax_orient.plot([], [], [], "r-", linewidth=3)[0],  # X
    ax_orient.plot([], [], [], "g-", linewidth=3)[0],  # Y
    ax_orient.plot([], [], [], "b-", linewidth=3)[0],  # Z
]
axis_vectors = np.array(
    [
        [[0, 0, 0], [0.7, 0, 0]],
        [[0, 0, 0], [0, 0.7, 0]],
        [[0, 0, 0], [0, 0, 0.7]],
    ]
)

(temp_line,) = ax_temp.plot([], [], color="orange", linewidth=1.5)
temp_label = ax_temp.text(0.02, 0.95, "", transform=ax_temp.transAxes, fontsize=10)
ax_temp.set_xlim(0, TEMP_WINDOW_S)
info_text = ax_info.text(
    0.0,
    0.95,
    "",
    transform=ax_info.transAxes,
    fontsize=11,
    family="monospace",
    va="top",
    ha="left",
)


def update(_):
    # Snapshot orientation and raw data
    with _lock:
        R_now = R.copy()
        a_now = _last_accel.copy()
        g_now = _last_gyro_dps.copy()
        temp_points = list(_temp_history)
        temp_now = _last_temp_c

    # Rotate geometry (body -> world)
    rotated_cube = cube @ R_now.T
    for i, e in enumerate(edges):
        xs, ys, zs = zip(rotated_cube[e[0]], rotated_cube[e[1]])
        lines[i].set_data(xs, ys)
        lines[i].set_3d_properties(zs)

    for i in range(3):
        rotated_axis = axis_vectors[i] @ R_now.T
        xs, ys, zs = rotated_axis[:, 0], rotated_axis[:, 1], rotated_axis[:, 2]
        axis_lines[i].set_data(xs, ys)
        axis_lines[i].set_3d_properties(zs)

    # HUD: quick Euler from R (for display only)
    pitch = math.degrees(math.atan2(-R_now[2, 1], R_now[2, 2]))
    roll = math.degrees(
        math.atan2(R_now[2, 0], math.sqrt(R_now[2, 1] ** 2 + R_now[2, 2] ** 2))
    )
    yaw = math.degrees(math.atan2(R_now[1, 0], R_now[0, 0]))

    temp_text = "n/a"
    if not math.isnan(temp_now):
        temp_text = f"{temp_now:5.2f}°C"

    # Update temperature plot
    if temp_points:
        latest_time = temp_points[-1][0]
        window_start = latest_time - TEMP_WINDOW_S
        filtered = [(t, v) for t, v in temp_points if t >= window_start]
        if filtered:
            times = [t for t, _ in filtered]
            temps = [v for _, v in filtered]
            t0 = times[0]
            xs_temp = [t - t0 for t in times]
            temp_line.set_data(xs_temp, temps)
            if xs_temp:
                x_max = max(xs_temp[-1], TEMP_WINDOW_S * 0.25)
                ax_temp.set_xlim(0, max(TEMP_WINDOW_S, x_max))
            temp_min = min(temps)
            temp_max = max(temps)
            if temp_min == temp_max:
                temp_min -= 0.5
                temp_max += 0.5
            margin = max(0.5, 0.1 * (temp_max - temp_min))
            ax_temp.set_ylim(temp_min - margin, temp_max + margin)
        else:
            temp_line.set_data([], [])
    else:
        temp_line.set_data([], [])

    temp_label.set_text(f"Latest: {temp_text}")

    info_text.set_text(
        "Orientation\n"
        f"  Pitch : {pitch:6.2f}°\n"
        f"  Roll  : {roll:6.2f}°\n"
        f"  Yaw   : {yaw:6.2f}°\n"
        "\n"
        "Acceleration (g)\n"
        f"  ax: {a_now[0]:6.3f}\n"
        f"  ay: {a_now[1]:6.3f}\n"
        f"  az: {a_now[2]:6.3f}\n"
        "\n"
        "Gyro (°/s)\n"
        f"  gx: {g_now[0]:6.2f}\n"
        f"  gy: {g_now[1]:6.2f}\n"
        f"  gz: {g_now[2]:6.2f}\n"
        "\n"
        f"Temperature\n  {temp_text}"
    )
    return lines + axis_lines + [info_text, temp_line, temp_label]


def _on_close(_):
    global _running
    _running = False


fig.canvas.mpl_connect("close_event", _on_close)

ani = FuncAnimation(
    fig, update, interval=int(UI_DT * 1000), blit=False, cache_frame_data=False
)
plt.show()

# Keep reference alive
import sys

sys.modules[__name__].__dict__["_ani_ref"] = ani
