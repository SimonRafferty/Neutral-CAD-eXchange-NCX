#!/bin/sh
# Build + deploy the add-in and reload the implementation in the running Fusion; fails loudly on errors.
cd "$(dirname "$0")/../.." || exit 1
out=$(powershell -ExecutionPolicy Bypass -File fusion/build.ps1 2>&1)
if echo "$out" | grep -q " error "; then echo "$out" | grep " error " | head -20; exit 1; fi
echo "$out" | grep -E "OK:|deployed|in use"
"${PYTHON:-python}" fusion/tools/fjob.py reload | grep -E '"ok"|error'
