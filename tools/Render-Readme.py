"""Render the release README using Markdown 3.7 installed in build/release-python."""
from pathlib import Path
import re
import sys

root = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(root / 'build' / 'release-python'))
try:
    import markdown
except ImportError:
    raise SystemExit('Install the release renderer: py -3 -m pip install --target build/release-python Markdown==3.7')

body = markdown.markdown((root / 'README.md').read_text(encoding='utf-8-sig'),
                         extensions=['tables', 'fenced_code', 'sane_lists'])
base = 'https://github.com/pancreations/nv3d-vision-restoration/blob/v0.1.0/'
body = re.sub(r'href="(?!https?://|#|mailto:)([^"]+)"', lambda m: 'href="' + base + m[1] + '"', body)
style = '''@page {size: Letter; margin: .65in} body {font: 10pt/1.4 "Segoe UI",sans-serif;color:#172016}
h1 {font-size:24pt} h2 {font-size:17pt;border-bottom:1pt solid #76b900;margin-top:20pt}
h3 {font-size:12pt} h1,h2,h3 {break-after:avoid} p,li {orphans:3;widows:3}
table {border-collapse:collapse;width:100%;font-size:9pt} th,td {border:1px solid #ccd4c5;padding:5pt;text-align:left}
tr {break-inside:avoid} thead {display:table-header-group} pre {white-space:pre-wrap;background:#eef3e7;padding:8pt}
code {font-family:Consolas,monospace;font-size:9pt;overflow-wrap:anywhere} a {color:#205a89;overflow-wrap:anywhere}
blockquote {border-left:4pt solid #76b900;background:#eef3e7;margin:12pt 0;padding:6pt 12pt}'''
destination = Path(sys.argv[1])
destination.write_text('<!doctype html><html lang="en"><meta charset="utf-8"><title>Vision Restoration README — v0.1.0</title><style>' + style + '</style><body>' + body + '</body></html>', encoding='utf-8')
