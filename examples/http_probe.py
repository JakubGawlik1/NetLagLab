#!/usr/bin/env python3
"""A single-process HTTPS workload for the NetLagLab CLI demonstration."""

import argparse
from contextlib import nullcontext
import http.client
import time
import urllib.error
import urllib.request


URL = "https://example.com/"


def report(message, log):
    print(message, flush=True)
    if log is not None:
        print(message, file=log, flush=True)


def run(log):
    # A host proxy could hide the application's direct network path.
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
    report(f"HTTPS workload: {URL}", log)
    report("Two-second pause between requests; stop with the Controller or Ctrl-C.",
           log)
    attempt = 0
    try:
        while True:
            attempt += 1
            started = time.monotonic()
            try:
                with opener.open(URL, timeout=5) as response:
                    body = response.read(65536)
                    result = f"HTTP {response.status}, {len(body)} bytes"
            except (urllib.error.URLError, OSError, http.client.HTTPException) as error:
                result = f"FAILED: {error}"
            elapsed_ms = (time.monotonic() - started) * 1000
            report(f"{attempt:03d}  {elapsed_ms:8.1f} ms  {result}", log)
            time.sleep(2)
    except KeyboardInterrupt:
        report("Workload stopped.", log)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--log", help="also write the workload output to this file")
    args = parser.parse_args()
    # Keep the optional file open for the run and close it on errors or Ctrl-C.
    with open(args.log, "w", encoding="utf-8") if args.log else nullcontext() as log:
        run(log)


if __name__ == "__main__":
    main()
