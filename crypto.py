#!/usr/bin/env python3
"""
USRL Crypto: Engine + Streamlit Web UI (single file)

Run UI:
  streamlit run crypto_web.py --server.address=0.0.0.0 --server.port=8501

Optional engine-only (no Streamlit):
  python crypto_web.py engine

Design goals:
- UI must NOT auto-start engine on import (Streamlit reruns would duplicate it).
- Engine stop must work under Streamlit (no OS signal handlers required).
  -> Use a USRL control topic (engine.control) that sets stop_event.
"""

import os
import time
import json
import random
import threading
import multiprocessing as mp
from collections import deque, defaultdict

import requests
from usrl import USRL


# -----------------------------
# CONFIG
# -----------------------------
COINS = [
    "BTC-USD","ETH-USD","LTC-USD","BCH-USD","LINK-USD",
    "DOT-USD","ADA-USD","XRP-USD","SOL-USD","MATIC-USD",
    "DOGE-USD","UNI-USD","AAVE-USD","MKR-USD","SUSHI-USD",
    "ATOM-USD","FIL-USD","ICP-USD","ALGO-USD","XTZ-USD"
]

COINBASE_TICKER = "https://api.exchange.coinbase.com/products/{}/ticker"

POLL_INTERVAL = 0.5

PUBLISH_TOPIC    = "market.ticks"
SIGNAL_TOPIC     = "market.signals"
ANALYTICS_TOPIC  = "market.analytics"
PORTFOLIO_TOPIC  = "portfolio.state"
CONTROL_TOPIC    = "engine.control"   # STOP channel for Streamlit-friendly shutdown

# Strategy (faster + "crazier", but bounded)
SHORT_EMA = 5
LONG_EMA  = 13

INITIAL_CASH   = 100000.0
ORDER_SIZE_PCT = 0.05

# Brokerage (simulated taker fee on notional).
# Coinbase maker/taker can be obtained via authenticated /fees; here we use a constant. [web:60]
TAKER_FEE_RATE = 0.006  # 0.60%

# Anti-noise + risk
COOLDOWN_SEC    = 2.0
TRAIL_STOP_PCT  = 0.015
VOL_WINDOW      = 32
VOL_ENTRY_MAG   = 0.00030

# USRL ring params (be generous for stress)
TICK_MSG_SIZE = 2048
SIG_MSG_SIZE  = 1024
ANA_MSG_SIZE  = 2048
PF_MSG_SIZE   = 4096
CTL_MSG_SIZE  = 256


# -----------------------------
# Helpers
# -----------------------------
def now_ns() -> int:
    return time.time_ns()

def embed_ts_json(obj: dict) -> bytes:
    ts = now_ns().to_bytes(8, "little")
    return ts + json.dumps(obj, separators=(",", ":")).encode("utf-8")

def unpack_ts_json(buf: bytes):
    if not buf or len(buf) < 8:
        return None, None
    ts = int.from_bytes(buf[:8], "little")
    try:
        body = json.loads(buf[8:].decode("utf-8"))
    except Exception:
        return ts, None
    return ts, body

def safe_float(x, default=None):
    try:
        return float(x)
    except Exception:
        return default

def ema_update(prev, price, period):
    if prev is None:
        return price
    alpha = 2 / (period + 1)
    return prev * (1 - alpha) + price * alpha


# -----------------------------
# Engine processes
# -----------------------------
def publisher_process(stop_event, use_sim):
    print("Publisher starting, sim=", use_sim, flush=True)

    usrl = USRL("publisher")
    pub = usrl.publisher(PUBLISH_TOPIC, slots=65536, size=TICK_MSG_SIZE)

    sim_prices = {c: random.uniform(10, 50000) for c in COINS}
    last_heartbeat = time.time()

    while not stop_event.is_set():
        batch = []

        if not use_sim:
            try:
                for sym in COINS:
                    r = requests.get(
                        COINBASE_TICKER.format(sym),
                        timeout=1.2,
                        headers={"User-Agent": "pyusrl-bench"}
                    )
                    if r.status_code == 200:
                        j = r.json()
                        price = safe_float(j.get("price"), 0.0) or 0.0
                        if price > 0:
                            batch.append({"symbol": sym, "price": price, "ts": now_ns(), "src": "coinbase"})
                    else:
                        raise RuntimeError("HTTP non-200")
            except Exception:
                print("Publisher: switching to simulation", flush=True)
                use_sim = True

        if use_sim:
            for sym in COINS:
                p = sim_prices[sym]
                p *= 1 + random.gauss(0, 0.0025)
                p = max(0.0001, p)
                sim_prices[sym] = p
                batch.append({"symbol": sym, "price": p, "ts": now_ns(), "src": "sim"})

        for b in batch:
            pub.send(embed_ts_json(b))

        if time.time() - last_heartbeat > 3:
            print(f"[PUB] published {len(batch)} ticks", flush=True)
            last_heartbeat = time.time()

        time.sleep(POLL_INTERVAL)

    pub.destroy()
    usrl.shutdown()
    print("Publisher exit", flush=True)


def analytics_process(stop_event):
    print("Analytics starting", flush=True)

    usrl = USRL("analytics")
    sub = usrl.subscriber(PUBLISH_TOPIC, buffer_size=1 << 20)
    pub_sig = usrl.publisher(SIGNAL_TOPIC, slots=16384, size=SIG_MSG_SIZE)
    pub_ana = usrl.publisher(ANALYTICS_TOPIC, slots=16384, size=ANA_MSG_SIZE)

    short = {c: None for c in COINS}
    long  = {c: None for c in COINS}

    last_cross_state = {c: 0 for c in COINS}  # -1 below, +1 above, 0 unknown
    last_trade_ts = {c: 0.0 for c in COINS}

    last_price = {c: None for c in COINS}
    rets = {c: deque(maxlen=VOL_WINDOW) for c in COINS}

    while not stop_event.is_set():
        buf = sub.recv()
        if buf is None:
            time.sleep(0.0005)
            continue

        _, payload = unpack_ts_json(buf)
        if not payload:
            continue

        sym = payload.get("symbol")
        price = safe_float(payload.get("price"), 0.0) or 0.0
        ts_src = payload.get("ts")

        if sym not in short or price <= 0:
            continue

        lp = last_price[sym]
        if lp is not None and lp > 0:
            rets[sym].append((price - lp) / lp)
        last_price[sym] = price

        vol_mag = 0.0
        if len(rets[sym]) >= 8:
            vol_mag = sum(abs(x) for x in rets[sym]) / len(rets[sym])

        short[sym] = ema_update(short[sym], price, SHORT_EMA)
        long[sym]  = ema_update(long[sym],  price, LONG_EMA)

        sig = "HOLD"
        if short[sym] is not None and long[sym] is not None:
            state = 1 if short[sym] > long[sym] else -1 if short[sym] < long[sym] else 0

            now_s = time.time()
            cooldown_ok = (now_s - last_trade_ts[sym]) >= COOLDOWN_SEC
            vol_ok = (vol_mag >= VOL_ENTRY_MAG)
            prev_state = last_cross_state[sym]

            # trade only on edge transitions (prevents BUY spam)
            if cooldown_ok and vol_ok:
                if prev_state <= 0 and state == 1:
                    sig = "BUY"
                    last_trade_ts[sym] = now_s
                elif prev_state >= 0 and state == -1:
                    sig = "SELL"
                    last_trade_ts[sym] = now_s

            last_cross_state[sym] = state

        out_sig = {
            "symbol": sym, "price": price, "signal": sig,
            "vol_mag": vol_mag, "ts_src": ts_src, "ts_ana": now_ns(),
        }
        pub_sig.send(embed_ts_json(out_sig))

        out_ana = {
            "symbol": sym, "price": price,
            "short_ema": short[sym], "long_ema": long[sym],
            "signal": sig, "vol_mag": vol_mag,
            "ts_src": ts_src, "ts_ana": now_ns(),
        }
        pub_ana.send(embed_ts_json(out_ana))

    sub.destroy()
    usrl.shutdown()
    print("Analytics exit", flush=True)


def portfolio_process(stop_event):
    print("Portfolio starting", flush=True)

    usrl = USRL("portfolio")
    sub = usrl.subscriber(SIGNAL_TOPIC, buffer_size=1 << 20)
    pub_state = usrl.publisher(PORTFOLIO_TOPIC, slots=8192, size=PF_MSG_SIZE)

    cash = float(INITIAL_CASH)
    realized = 0.0
    fees_paid = 0.0

    positions = {}   # sym -> {qty, avg, peak}
    last_price = {}

    last_print = time.time()
    last_state_pub = 0.0

    def mtm_equity():
        eq = cash
        for s, pos in positions.items():
            px = last_price.get(s, pos["avg"])
            eq += pos["qty"] * px
        return eq

    while not stop_event.is_set():
        buf = sub.recv()
        if buf is None:
            time.sleep(0.0005)
            continue

        _, p = unpack_ts_json(buf)
        if not p:
            continue

        sym = p.get("symbol")
        price = safe_float(p.get("price"), 0.0) or 0.0
        sig = p.get("signal", "HOLD")

        if sym is None or price <= 0:
            continue

        last_price[sym] = price

        # trailing stop (forces sell if drawdown from peak)
        if sym in positions:
            pos = positions[sym]
            pos["peak"] = max(pos["peak"], price)
            if pos["peak"] > 0 and (price / pos["peak"] - 1.0) <= -TRAIL_STOP_PCT:
                sig = "SELL"

        if sig == "BUY":
            invest = cash * ORDER_SIZE_PCT
            if invest > 5:
                fee = invest * TAKER_FEE_RATE
                total = invest + fee
                if total <= cash:
                    qty = invest / price
                    cash -= total
                    fees_paid += fee
                    realized -= fee

                    if sym in positions:
                        pos = positions[sym]
                        new_qty = pos["qty"] + qty
                        if new_qty > 0:
                            pos["avg"] = (pos["qty"] * pos["avg"] + qty * price) / new_qty
                        pos["qty"] = new_qty
                        pos["peak"] = max(pos["peak"], price)
                    else:
                        positions[sym] = {"qty": qty, "avg": price, "peak": price}

                    print(f"[PORT] BUY {sym} qty={qty:.6f} @ {price:.4f} fee={fee:.2f}", flush=True)

        elif sig == "SELL":
            if sym in positions:
                pos = positions.pop(sym)
                proceeds = pos["qty"] * price
                fee = proceeds * TAKER_FEE_RATE
                net = proceeds - fee
                fees_paid += fee

                pnl = net - (pos["qty"] * pos["avg"])
                realized += pnl
                cash += net

                print(f"[PORT] SELL {sym} pnl={pnl:.2f} fee={fee:.2f}", flush=True)

        # Periodic prints
        if time.time() - last_print > 4:
            equity = mtm_equity()
            print(f"[PORT] cash={cash:.2f} pos={len(positions)} realized={realized:.2f} fees={fees_paid:.2f} equity≈{equity:.2f}", flush=True)
            last_print = time.time()

        # Publish state for UI (throttle)
        now_s = time.time()
        if (now_s - last_state_pub) > 0.25:
            equity = mtm_equity()
            state = {
                "ts": now_ns(),
                "cash": cash,
                "equity": equity,
                "realized": realized,
                "fees_paid": fees_paid,
                "positions": {s: {"qty": v["qty"], "avg": v["avg"], "peak": v["peak"]} for s, v in positions.items()},
                "last_price": {s: last_price.get(s, None) for s in positions.keys()},
            }
            pub_state.send(embed_ts_json(state))
            last_state_pub = now_s

    sub.destroy()
    usrl.shutdown()
    print("Portfolio exit", flush=True)


# -----------------------------
# Engine runner (Streamlit-safe stop via CONTROL_TOPIC)
# -----------------------------
def run_engine_blocking():
    print("Booting USRL engine…", flush=True)

    stop_event = mp.Event()

    # Decide SIM vs Coinbase
    use_sim = False
    try:
        r = requests.get(COINBASE_TICKER.format("BTC-USD"), timeout=1)
        if r.status_code != 200:
            use_sim = True
    except Exception:
        use_sim = True
    print("Coinbase OK:" if not use_sim else "Coinbase unreachable → SIM mode", flush=True)

    # Control subscriber (STOP message)
    ctl = USRL("engine_ctl")
    ctl_sub = ctl.subscriber(CONTROL_TOPIC, buffer_size=4096)

    procs = []
    p_pub = mp.Process(target=publisher_process, args=(stop_event, use_sim), daemon=True); p_pub.start(); procs.append(p_pub)
    time.sleep(0.6)
    p_ana = mp.Process(target=analytics_process, args=(stop_event,), daemon=True); p_ana.start(); procs.append(p_ana)
    time.sleep(0.4)
    p_pf  = mp.Process(target=portfolio_process, args=(stop_event,), daemon=True); p_pf.start(); procs.append(p_pf)

    try:
        while any(p.is_alive() for p in procs):
            # poll control
            buf = ctl_sub.recv()
            if buf:
                _, msg = unpack_ts_json(buf)
                if isinstance(msg, dict) and msg.get("cmd") == "STOP":
                    print("Engine received STOP command.", flush=True)
                    stop_event.set()
                    break
            time.sleep(0.05)
    finally:
        stop_event.set()
        for p in procs:
            p.join(timeout=2.0)
        ctl_sub.destroy()
        ctl.shutdown()

    print("Engine exit clean", flush=True)


# -----------------------------
# Streamlit UI
# -----------------------------
def run_streamlit_ui():
    import streamlit as st
    import pandas as pd
    import plotly.graph_objects as go

    st.set_page_config(page_title="USRL Crypto Web", layout="wide")
    st.title("USRL Crypto Web App (USRL-only IPC)")

    with st.sidebar:
        st.header("Runtime")
        refresh_ms = st.slider("Refresh (ms)", 100, 2000, 400, 50)
        max_points = st.slider("Chart points/symbol", 200, 10000, 2500, 100)
        drain_max  = st.slider("Drain per refresh/topic", 100, 20000, 5000, 100)
        show_hold  = st.checkbox("Show HOLD signals", value=False)

        st.divider()
        st.subheader("Engine (same container)")
        if "engine_started" not in st.session_state:
            st.session_state.engine_started = False
        start = st.button("Start engine")
        stop  = st.button("Stop engine")

    # Engine control publisher (UI -> engine)
    @st.cache_resource
    def make_ctl_pub():
        u = USRL("ui_ctl")
        pub = u.publisher(CONTROL_TOPIC, slots=64, size=CTL_MSG_SIZE)
        return u, pub

    _, ctl_pub = make_ctl_pub()

    # Start engine once (guard against reruns)
    if start and not st.session_state.engine_started:
        st.session_state.engine_started = True
        t = threading.Thread(target=run_engine_blocking, daemon=True)
        t.start()

    if stop:
        ctl_pub.send(embed_ts_json({"cmd": "STOP"}))
        st.session_state.engine_started = False

    # Subscribers
    @st.cache_resource
    def make_subs():
        u = USRL("ui")
        sub_ana = u.subscriber(ANALYTICS_TOPIC, buffer_size=1 << 20)
        sub_sig = u.subscriber(SIGNAL_TOPIC, buffer_size=1 << 20)
        sub_pf  = u.subscriber(PORTFOLIO_TOPIC, buffer_size=1 << 20)
        return u, sub_ana, sub_sig, sub_pf

    _, sub_ana, sub_sig, sub_pf = make_subs()

    # State buffers
    if "series" not in st.session_state:
        st.session_state.series = defaultdict(lambda: deque(maxlen=max_points))
    if "signals" not in st.session_state:
        st.session_state.signals = deque(maxlen=8000)
    if "pf" not in st.session_state:
        st.session_state.pf = None

    # Resize deques if needed
    for sym, dq in list(st.session_state.series.items()):
        if dq.maxlen != max_points:
            st.session_state.series[sym] = deque(dq, maxlen=max_points)

    def drain(sub, handler):
        n = 0
        while n < drain_max:
            buf = sub.recv()
            if not buf:
                break
            _, p = unpack_ts_json(buf)
            if p:
                handler(p)
            n += 1
        return n

    def on_ana(p):
        sym = p.get("symbol")
        if not sym:
            return
        t = p.get("ts_ana") or p.get("ts_src") or now_ns()
        st.session_state.series[sym].append({
            "t": int(t) / 1e9,
            "symbol": sym,
            "price": safe_float(p.get("price"), 0.0) or 0.0,
            "short_ema": safe_float(p.get("short_ema"), None),
            "long_ema": safe_float(p.get("long_ema"), None),
            "signal": p.get("signal", "HOLD"),
            "vol_mag": safe_float(p.get("vol_mag"), 0.0) or 0.0,
        })

    def on_sig(p):
        sig = p.get("signal", "HOLD")
        if (not show_hold) and sig == "HOLD":
            return
        sym = p.get("symbol")
        if not sym:
            return
        t = p.get("ts_ana") or p.get("ts_src") or now_ns()
        st.session_state.signals.append({
            "t": int(t) / 1e9,
            "symbol": sym,
            "signal": sig,
            "price": safe_float(p.get("price"), 0.0) or 0.0,
            "vol_mag": safe_float(p.get("vol_mag"), 0.0) or 0.0,
        })

    def on_pf(p):
        st.session_state.pf = p

    n_ana = drain(sub_ana, on_ana)
    n_sig = drain(sub_sig, on_sig)
    n_pf  = drain(sub_pf,  on_pf)

    left, right = st.columns([2.2, 1.0])

    with right:
        st.subheader("Status")
        st.write(f"drained: analytics={n_ana}, signals={n_sig}, portfolio={n_pf}")

        st.subheader("Portfolio")
        pf = st.session_state.pf or {}
        st.metric("Equity", f"{(safe_float(pf.get('equity'), 0.0) or 0.0):,.2f}")
        st.metric("Cash", f"{(safe_float(pf.get('cash'), 0.0) or 0.0):,.2f}")
        st.metric("Realized", f"{(safe_float(pf.get('realized'), 0.0) or 0.0):,.2f}")
        st.metric("Fees", f"{(safe_float(pf.get('fees_paid'), 0.0) or 0.0):,.2f}")

        pos = pf.get("positions") or {}
        lp  = pf.get("last_price") or {}
        if pos:
            dfp = pd.DataFrame([
                {
                    "symbol": s,
                    "qty": safe_float(v.get("qty"), 0.0) or 0.0,
                    "avg": safe_float(v.get("avg"), 0.0) or 0.0,
                    "last": safe_float(lp.get(s), None),
                    "peak": safe_float(v.get("peak"), 0.0) or 0.0,
                }
                for s, v in pos.items()
            ])
            if not dfp.empty:
                dfp["uPnL"] = (dfp["last"] - dfp["avg"]) * dfp["qty"]
                st.dataframe(dfp.sort_values("symbol"), use_container_width=True, height=260)
        else:
            st.caption("No open positions (or engine not started).")

    with left:
        st.subheader("Price + EMA + Signals")
        symbols = sorted(st.session_state.series.keys())
        if not symbols:
            st.info("Waiting for market.analytics ... (start engine or run engine container)")
        else:
            sym = st.selectbox("Symbol", symbols, index=0)
            df = pd.DataFrame(list(st.session_state.series[sym]))

            fig = go.Figure()
            if not df.empty:
                fig.add_trace(go.Scatter(x=df["t"], y=df["price"], name="Price", mode="lines"))
                if df["short_ema"].notna().any():
                    fig.add_trace(go.Scatter(x=df["t"], y=df["short_ema"], name="EMA(short)", mode="lines"))
                if df["long_ema"].notna().any():
                    fig.add_trace(go.Scatter(x=df["t"], y=df["long_ema"], name="EMA(long)", mode="lines"))

                sigs = [s for s in st.session_state.signals if s["symbol"] == sym and s["signal"] in ("BUY","SELL")]
                if sigs:
                    sdf = pd.DataFrame(sigs)
                    fig.add_trace(go.Scatter(
                        x=sdf["t"], y=sdf["price"], mode="markers",
                        name="Signals", text=sdf["signal"], marker=dict(size=10)
                    ))

            fig.update_layout(height=560, margin=dict(l=10, r=10, t=30, b=10))
            st.plotly_chart(fig, use_container_width=True)

            st.subheader("Latest analytics")
            st.dataframe(df.tail(80), use_container_width=True, height=260)

    time.sleep(refresh_ms / 1000.0)
    st.rerun()  # Streamlit rerun is the intended way to refresh live pages. [web:43]


# -----------------------------
# Entrypoint
# -----------------------------
if __name__ == "__main__":
    # If invoked by Streamlit: do UI. If invoked directly: allow engine mode.
    mode = "ui"
    if len(os.sys.argv) >= 2 and os.sys.argv[1].strip().lower() == "engine":
        mode = "engine"

    if mode == "engine":
        run_engine_blocking()
    else:
        run_streamlit_ui()
