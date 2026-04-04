#!/usr/bin/env python3
from __future__ import annotations

import argparse
from pathlib import Path

import matplotlib.pyplot as plt
import pandas as pd


NUMERIC_COLUMNS = [
    "timestamp_ms",
    "session_id",
    "size_bytes",
    "hops",
    "exits",
    "elapsed_ms",
    "progress_pct",
    "sent_bytes",
    "tx_Bps",
    "acked_frags",
    "ack_fps",
    "inflight",
    "max_inflight",
    "mesh_retx",
    "oow_queued",
    "oow_sent",
    "cong",
    "bp",
    "mesh_ms",
    "cloud_wait_ms",
    "e2e_Bps",
    "mesh_Bps",
    "total_ms",
]


def col_or_zeros(df: pd.DataFrame, name: str) -> pd.Series:
    if name in df.columns:
        return df[name]
    return pd.Series([0] * len(df), index=df.index)


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Plot FLP telemetry time-series and final summary charts."
    )
    parser.add_argument(
        "--csv",
        type=Path,
        default=Path(__file__).with_name("flp_telemetry_7AE0.csv"),
        help="Path to telemetry CSV (default: evalulation/flp_telemetry_7AE0.csv)",
    )
    parser.add_argument(
        "--out-dir",
        type=Path,
        default=None,
        help="Directory for output plots (default: <csv_parent>/plots)",
    )
    return parser


def main() -> int:
    args = build_parser().parse_args()
    csv_path = args.csv.expanduser().resolve()
    if not csv_path.exists():
        raise FileNotFoundError(f"CSV file not found: {csv_path}")

    out_dir = args.out_dir or (csv_path.parent / "plots")
    out_dir.mkdir(parents=True, exist_ok=True)

    df = pd.read_csv(csv_path)
    for col in NUMERIC_COLUMNS:
        if col in df.columns:
            df[col] = pd.to_numeric(df[col], errors="coerce")

    periodic = df[df["result"] == "periodic"].copy()
    finals = df[df["result"] == "complete"].copy()

    # Per-session time-series charts.
    for sid, group in periodic.groupby("session_id"):
        group = group.sort_values("elapsed_ms").copy()
        t = group["elapsed_ms"] / 1000.0
        sid_label = int(sid) if pd.notna(sid) else "unknown"

        fig, axs = plt.subplots(4, 1, figsize=(10, 12), sharex=True)

        axs[0].plot(t, group["tx_Bps"], label="tx_Bps")
        axs[0].set_ylabel("Bytes/s")
        axs[0].set_title(f"Session {sid_label} - Throughput")
        axs[0].grid(True)
        axs[0].legend()

        axs[1].plot(t, group["acked_frags"], label="acked_frags")
        axs[1].plot(t, group["inflight"], label="inflight")
        axs[1].set_ylabel("Count")
        axs[1].set_title("ACK progress and in-flight window")
        axs[1].grid(True)
        axs[1].legend()

        axs[2].plot(t, group["mesh_retx"], label="mesh_retx")
        axs[2].plot(t, group["oow_queued"], label="oow_queued")
        axs[2].plot(t, group["oow_sent"], label="oow_sent")
        axs[2].set_ylabel("Count")
        axs[2].set_title("Recovery / retransmission pressure")
        axs[2].grid(True)
        axs[2].legend()

        axs[3].plot(t, group["cong"], label="cong")
        axs[3].plot(t, group["bp"], label="bp")
        axs[3].set_ylabel("Count")
        axs[3].set_xlabel("Elapsed time (s)")
        axs[3].set_title("Congestion / backpressure")
        axs[3].grid(True)
        axs[3].legend()

        plt.tight_layout()
        fig.savefig(
            out_dir / f"session_{sid_label}_timeseries.png",
            dpi=160,
            bbox_inches="tight",
        )
        plt.close(fig)

    if not finals.empty:
        finals = finals.sort_values("timestamp_ms").copy()
        finals.to_csv(out_dir / "final_summary.csv", index=False)

        completion_col = "total_ms" if "total_ms" in finals.columns else "elapsed_ms"
        session_labels = finals["session_id"].fillna(-1).astype(int).astype(str)

        fig, axs = plt.subplots(2, 2, figsize=(12, 8))

        axs[0, 0].bar(session_labels, col_or_zeros(finals, completion_col))
        axs[0, 0].set_title("Total completion time")
        axs[0, 0].set_ylabel("ms")
        axs[0, 0].tick_params(axis="x", rotation=45)

        axs[0, 1].bar(session_labels, col_or_zeros(finals, "e2e_Bps"))
        axs[0, 1].set_title("End-to-end throughput")
        axs[0, 1].set_ylabel("Bytes/s")
        axs[0, 1].tick_params(axis="x", rotation=45)

        axs[1, 0].bar(session_labels, col_or_zeros(finals, "mesh_Bps"))
        axs[1, 0].set_title("Mesh throughput")
        axs[1, 0].set_ylabel("Bytes/s")
        axs[1, 0].tick_params(axis="x", rotation=45)

        axs[1, 1].bar(session_labels, col_or_zeros(finals, "max_inflight"))
        axs[1, 1].set_title("Peak in-flight window")
        axs[1, 1].set_ylabel("Fragments")
        axs[1, 1].tick_params(axis="x", rotation=45)

        plt.tight_layout()
        fig.savefig(out_dir / "final_summary_bars.png",
                    dpi=160, bbox_inches="tight")
        plt.close(fig)

    print(f"Wrote plots to: {out_dir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
