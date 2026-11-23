"""Downloads building outlines from OpenStreetMap for an area (names, heights, year of construction, building parts).

Usage: fetch_outlines.py <west> <south> <east> <north> <output.json>
West to north in degrees.
Source: © OpenStreetMap contributors, Open Database License.
"""
import json
import sys

import requests

w, s, e, n = sys.argv[1:5]
q = f"""[out:json][timeout:300];
(
  way["building"]({s},{w},{n},{e});
  way["building:part"]({s},{w},{n},{e});
  relation["building"]({s},{w},{n},{e});
  relation["building:part"]({s},{w},{n},{e});
);
out tags geom;"""
for host in ('https://overpass-api.de/api/interpreter', 'https://overpass.kumi.systems/api/interpreter'):
    try:
        r = requests.post(host, data={'data': q}, timeout=400, headers={'User-Agent': 'sunbeam-tools/1.0'})
        if r.status_code == 200:
            break
        print(host, r.status_code, flush=True)
    except Exception as ex:
        print(host, ex, flush=True)
d = r.json()
open(sys.argv[5], 'w', encoding='utf-8').write(json.dumps(d))
print(len(d['elements']), 'outlines')
