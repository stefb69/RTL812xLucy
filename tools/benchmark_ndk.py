#!/usr/bin/env python3
"""Record an interface-bound iperf3 TCP matrix without changing network settings."""
import argparse
import csv
import datetime as dt
import json
import math
import platform
import shlex
import subprocess
from pathlib import Path


def number(value, name, required=False):
    if value is None and not required:
        return None
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise ValueError(f"missing or nonnumeric {name}")
    if not math.isfinite(value) or value < 0:
        raise ValueError(f"invalid {name}: {value}")
    return value


def metrics(payload, direction):
    """TX is the local sender; reverse/RX is the local receiver."""
    if direction not in ("tx", "rx"):
        raise ValueError(f"invalid direction: {direction}")
    if not isinstance(payload, dict) or payload.get("error"):
        raise ValueError(str(payload.get("error")) if isinstance(payload, dict) else "JSON is not an object")
    end = payload.get("end")
    if not isinstance(end, dict):
        raise ValueError("missing end results")
    sent, received = end.get("sum_sent", {}), end.get("sum_received", {})
    cpu = end.get("cpu_utilization_percent", {})
    if not all(isinstance(item, dict) for item in (sent, received, cpu)):
        raise ValueError("invalid summary objects")
    selected = sent if direction == "tx" else received
    return {
        "local_role": "sender" if direction == "tx" else "receiver",
        "local_bps": number(selected.get("bits_per_second"), "local throughput", True),
        "sender_bps": number(sent.get("bits_per_second"), "sender throughput"),
        "receiver_bps": number(received.get("bits_per_second"), "receiver throughput"),
        "retransmits": number(sent.get("retransmits"), "sender retransmits"),
        "host_cpu_percent": number(cpu.get("host_total"), "host CPU"),
        "remote_cpu_percent": number(cpu.get("remote_total"), "remote CPU"),
    }


def run(command, timeout):
    result = {"command": command, "returncode": None, "stdout": "", "stderr": "", "error": None}
    try:
        process = subprocess.run(command, capture_output=True, text=True, timeout=timeout)
        result.update(returncode=process.returncode, stdout=process.stdout, stderr=process.stderr)
        if process.returncode:
            result["error"] = f"exit status {process.returncode}"
    except subprocess.TimeoutExpired as exc:
        result.update(error=f"timeout after {timeout}s", stdout=exc.stdout or "", stderr=exc.stderr or "")
    except OSError as exc:
        result["error"] = str(exc)
    for key in ("stdout", "stderr"):
        if isinstance(result[key], bytes):
            result[key] = result[key].decode("utf-8", errors="replace")
    return result


def write_json(path, value):
    path.write_text(json.dumps(value, indent=2, allow_nan=False) + "\n")


def invalid_constant(token):
    raise ValueError(f"nonstandard JSON constant: {token}")


def finite_float(token):
    value = float(token)
    if not math.isfinite(value):
        raise ValueError(f"nonfinite JSON number: {token}")
    return value


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", required=True, help="iperf3 server address")
    parser.add_argument("--interface", required=True, help="local interface, e.g. en10")
    parser.add_argument("-t", "--time", type=int, default=10)
    parser.add_argument("-O", "--omit", type=int, default=2)
    parser.add_argument("--reps", type=int, default=2)
    parser.add_argument("--streams", type=int, nargs="+", default=[1, 2, 4, 8])
    parser.add_argument("--port", type=int, default=5201)
    parser.add_argument("--iperf3", default="iperf3")
    parser.add_argument("--output", type=Path, default=Path("ndk-benchmark-" + dt.datetime.now(dt.timezone.utc).strftime("%Y%m%dT%H%M%SZ")))
    parser.add_argument("--dry-run", action="store_true", help="print commands; create no files or traffic")
    args = parser.parse_args()
    if args.time < 1 or args.omit < 0 or args.reps < 1 or min(args.streams) < 1 or not 1 <= args.port <= 65535:
        parser.error("time, reps and streams must be positive; omit >= 0; port between 1 and 65535")
    snapshots = [["netstat", "-I", args.interface, "-qq"], ["ifconfig", args.interface]]
    jobs = []
    for rep in range(1, args.reps + 1):
        for streams in args.streams:
            for direction in ("tx", "rx"):
                command = [args.iperf3, "-c", args.host, "--bind-dev", args.interface, "-p", str(args.port),
                           "-J", "-t", str(args.time), "-O", str(args.omit), "-P", str(streams),
                           "--connect-timeout", "5000", "--rcv-timeout", "5000"]
                jobs.append((rep, streams, direction, command + (["-R"] if direction == "rx" else [])))
    if args.dry_run:
        print(json.dumps({"before_and_after_each_run": snapshots, "runs": [job[3] for job in jobs]}, indent=2))
        return 0
    try:
        args.output.mkdir(parents=True, exist_ok=False)
    except OSError as exc:
        parser.error(f"cannot create output directory: {exc}")
    write_json(args.output / "environment.json", {"platform": platform.platform(), "settings": vars(args) | {"output": str(args.output)},
               "iperf3_version": run([args.iperf3, "--version"], 5)})
    summary = []
    fields = ["rep", "streams", "direction", "status", "error", "local_role", "local_bps", "sender_bps", "receiver_bps",
              "retransmits", "host_cpu_percent", "remote_cpu_percent", "snapshot_errors", "artifact"]
    for index, (rep, streams, direction, command) in enumerate(jobs, 1):
        artifact = f"{index:02d}-r{rep}-p{streams}-{direction}.json"
        print(shlex.join(command), flush=True)
        record = {"started_utc": dt.datetime.now(dt.timezone.utc).isoformat(), "before": [run(cmd, 5) for cmd in snapshots]}
        record["process"] = process = run(command, args.time + args.omit + 20)
        record["after"] = [run(cmd, 5) for cmd in snapshots]
        row = dict.fromkeys(fields)
        row.update(rep=rep, streams=streams, direction=direction, status="failed", artifact=artifact)
        row["snapshot_errors"] = "; ".join(item["error"] for item in record["before"] + record["after"] if item["error"])
        try:
            record["iperf"] = json.loads(process["stdout"], parse_constant=invalid_constant, parse_float=finite_float)
            if process["error"]:
                raise ValueError(process["error"])
            row.update(metrics(record["iperf"], direction), status="ok")
        except (ValueError, TypeError) as exc:
            row["error"] = process["error"] or str(exc)
        record["summary"] = row
        write_json(args.output / artifact, record)
        summary.append(row)
        write_json(args.output / "summary.json", summary)
        with (args.output / "summary.csv").open("w", newline="") as stream:
            writer = csv.DictWriter(stream, fieldnames=fields)
            writer.writeheader()
            writer.writerows(summary)
        print(f"{artifact}: {row['status']} " + (f"{row['local_bps'] / 1e9:.3f} Gbit/s" if row["status"] == "ok" else row["error"]), flush=True)
    print(f"Results: {args.output}")
    return int(any(row["status"] != "ok" for row in summary))


if __name__ == "__main__":
    raise SystemExit(main())
