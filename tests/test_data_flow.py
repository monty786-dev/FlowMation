#!/usr/bin/env python3
"""Runs a browser-less flow with fbrun (control flow, text, Excel/CSV/JSON steps) and verifies the outputs
independently with openpyxl / csv / json."""
import csv, json, os, subprocess, sys, tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FBRUN = sys.argv[1] if len(sys.argv) > 1 else "/tmp/fbrun"
tmp = tempfile.mkdtemp(prefix="fbdata_")
subprocess.check_call([sys.executable, os.path.join(ROOT, "tests", "make_fixtures.py"), os.path.join(tmp, "customers.xlsx")], stdout=subprocess.DEVNULL)

def S(_t, **kw): d = {"type": _t}; d.update(kw); return d

flow = {
    "format": "flowbrowser-flow", "version": 1, "name": "data test",
    "variables": {"total": 0, "big": [], "x": "=1+1", "n": 0},
    "settings": {"on_error": "stop"},
    "steps": [
        S("excel_read", file="customers.xlsx", sheet="Customers", header=True, skip_empty=True, result_var="rows"),
        S("foreach", source="rows", **{"as": "row"}, index_as="i", body=[
            S("if", condition="{{row.Email}} contains \"@\"", **{"then": [
                S("calc", var="total", expression="{{total}} + {{row.Amount}}"),
                S("if", condition="{{row.Amount}} > 100", **{"then": [S("list_add", var="big", value="{{row.Name}}")]}),
                S("text_transform", value="{{row.Email}}", op="regex_extract", pattern="@(.+)$", group=1, result_var="domain"),
                S("excel_append_row", file="out.xlsx", sheet="Results", values=["{{i}}", "{{row.Name}}", "{{domain | upper}}", "=HYPERLINK(\"http://evil\")"]),
            ]}),
        ]),
        S("excel_write_cell", file="out.xlsx", sheet="Summary", cell="A1", value="Total"),
        S("excel_write_cell", file="out.xlsx", sheet="Summary", cell="B1", value="{{total}}", value_type="number"),
        S("excel_write_cell", file="out.xlsx", sheet="Summary", cell="B2", value="=SUM(B1:B1)"),
        S("excel_write_cell", file="out.xlsx", sheet="Summary", cell="B3", value="{{x}}"),          # data, must stay text
        S("excel_write_table", file="out.xlsx", sheet="Table", source="rows", start_cell="C3"),
        S("excel_write_table", file="out.xlsx", sheet="Table", source="rows", start_cell="C3", mode="append", headers=False),
        S("csv_write", file="out.csv", source="rows"),
        S("json_write", file="out.json", source="big"),
        S("text_write", file="log.txt", text="hello {{big.length}}", append=False),
        S("text_read", file="log.txt", result_var="logtxt"),
        S("try", body=[S("excel_read", file="missing.xlsx", result_var="nope")], catch=[S("set_var", var="caught", value="yes")], error_var="why"),
        S("repeat", times=5, index_as="k", body=[S("if", condition="{{k}} == 3", **{"then": [S("break")]}), S("calc", var="n", expression="{{n}} + 1")]),
        S("set_var", var="parts", value="[\"a\",\"b\"]", value_type="json"),
        S("text_transform", value="a1b22c333", op="regex_extract_all", pattern="\\d+", result_var="nums"),
        S("text_transform", value="2024-03-15", op="regex_replace", pattern="(\\d+)-(\\d+)-(\\d+)", replacement="$3/$2/$1", result_var="eu"),
        S("text_transform", value="a, b;c", op="split", pattern="[,;]\\s*", result_var="split"),
        S("while", condition="{{n}} < 10", max_iterations=50, body=[S("calc", var="n", expression="{{n}} + 4")]),
        S("excel_read", file="out.xlsx", sheet="Results", header=False, result_var="back"),
        S("stop", message="done"),
        S("set_var", var="unreachable", value="1"),
    ],
}
path = os.path.join(tmp, "data.fbflow")
json.dump(flow, open(path, "w"))
p = subprocess.run([FBRUN, path, "--catalog", os.path.join(ROOT, "src/core/catalog.json"), "--base", tmp],
                   capture_output=True, text=True, stdin=subprocess.DEVNULL, timeout=60)
events = [json.loads(l)["event"] for l in p.stdout.splitlines() if l.startswith('{"op":"emit"')]
fails = 0
def check(cond, msg):
    global fails
    if not cond:
        fails += 1; print("FAIL:", msg)
print("exit", p.returncode, "stderr:", p.stderr.strip()[:500])
check(p.returncode == 0, "fbrun exit code 0")
end = [e for e in events if e["ev"] == "flow_end"]
check(end and end[-1]["result"] == "ok", "flow_end ok: %r" % (end[-1:] ,))
V = [e for e in events if e["ev"] == "vars"][-1]["vars"]
check(abs(V["total"] - (1250.5 + 99 + 0.30000000000000004)) < 1e-6, "total=%r" % V["total"])
check(V["big"] == ["Ana Müller"], "big=%r" % V["big"])
check(V.get("caught") == "yes" and "missing.xlsx" in V.get("why", ""), "try/catch: %r %r" % (V.get("caught"), V.get("why")))
check(V["n"] == 10, "repeat/while n=%r" % V["n"])
check(V["nums"] == ["1", "22", "333"], "regex_extract_all %r" % V["nums"])
check(V["eu"] == "15/03/2024", "regex_replace %r" % V["eu"])
check(V["split"] == ["a", "b", "c"], "split %r" % V["split"])
check(V["logtxt"].strip() == "hello 1", "text %r" % V["logtxt"])
check("unreachable" not in V, "stop ends the flow")
check(V["parts"] == ["a", "b"], "json var")

from openpyxl import load_workbook
wb = load_workbook(os.path.join(tmp, "out.xlsx"))
check(wb.sheetnames == ["Results", "Summary", "Table"] or set(wb.sheetnames) >= {"Results", "Summary", "Table"}, "sheets %r" % wb.sheetnames)
r = list(wb["Results"].iter_rows(values_only=True))
check(len(r) == 3, "Results rows %r" % r)
check(r[0][:3] == (1, "Ana Müller", "EXAMPLE.COM"), "row1 %r" % (r[0],))
check(r[0][3] == '=HYPERLINK("http://evil")' and wb["Results"]["D1"].data_type != "f", "formula injection blocked: %r dt=%s" % (r[0][3], wb["Results"]["D1"].data_type))
s = wb["Summary"]
check(abs(s["B1"].value - 1349.8) < 1e-6, "Summary B1 %r" % s["B1"].value)
check(s["B2"].data_type == "f" and s["B2"].value == "=SUM(B1:B1)", "literal formula kept %r" % s["B2"].value)
check(s["B3"].data_type != "f" and s["B3"].value == "=1+1", "templated '=' value stays text %r %s" % (s["B3"].value, s["B3"].data_type))
t = list(wb["Table"].iter_rows(min_row=3, min_col=3, values_only=True))
check(t[0][:3] == ("Name", "Email", "Amount"), "table header %r" % (t[0],))
check(wb["Table"]["C3"].font.bold, "header bold")
check(len(t) == 1 + 5 + 5, "table rows (header + 5 + 5 appended) = %d" % len(t))
with open(os.path.join(tmp, "out.csv"), encoding="utf-8-sig", newline="") as f:
    rows = list(csv.reader(f))
check(rows[0][:3] == ["Name", "Email", "Amount"] and len(rows) == 6, "csv %r" % rows[:2])
check(json.load(open(os.path.join(tmp, "out.json"))) == ["Ana Müller"], "json file")
check(V["back"][0][:3] == ["1", "Ana Müller", "EXAMPLE.COM"] or V["back"][0][1] == "Ana Müller", "re-read %r" % (V["back"][:1],))
print("ALL OK" if not fails else "%d FAILED" % fails)
sys.exit(1 if fails else 0)
