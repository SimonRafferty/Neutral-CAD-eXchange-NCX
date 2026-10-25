#!/bin/sh
# developer: import a few parts of the folding step file (HG_FILE) with "only"; prints the non-rebuilt items
cd "$(dirname "$0")/../.." || exit 1
[ -n "$HG_FILE" ] || { echo "set HG_FILE to the NCX file"; exit 1; }
out=fusion/results/hengist/only_$(echo "$1" | tr -c 'A-Za-z0-9\n' '_').json
"${PYTHON:-python}" fusion/tools/gaps.py "$HG_FILE" "$out" "{\"only\": $1, \"trace\": ${2:-false}}" | head -3
"${PYTHON:-python}" - "$out" <<'PY'
import json,sys
r=json.load(open(sys.argv[1],encoding='utf-8'))
for e in r.get('report',[]):
    if e['result']!='rebuilt': print(' ',e['document'][:22],'|',e['item'],'|',e['result'],'|',e.get('reason','')[:300])
for v in r.get('verification',[]): print(' V',{k:v[k] for k in v if k not in('massDetail','flexDetail')})
if not r.get('ok'): print(r)
PY
