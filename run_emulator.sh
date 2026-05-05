#!/bin/bash
# Run POM2 from the repo root so roms/ probes resolve.
cd "$(dirname "$0")"
./build/POM2 "$@"
