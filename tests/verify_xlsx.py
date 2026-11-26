#!/usr/bin/env python3
"""Independent verification (openpyxl) of what the C code wrote. Usage: verify_xlsx.py <out_dir>"""
import sys
import zipfile

from openpyxl import load_workbook

out = sys.argv[1]
ok = True


def check(cond, msg):
    global ok
    print(("  ok   " if cond else "  FAIL ") + msg)
    ok = ok and bool(cond)


print("patched.xlsx (existing workbook edited in place)")
wb = load_workbook(f"{out}/patched.xlsx")
check(wb.sheetnames == ["Customers", "Data & Notes", "Results_ 2026"], f"sheet names {wb.sheetnames}")
ws = wb["Customers"]
check(ws["A1"].font.bold and ws["A1"].fill.fgColor.rgb.endswith("E95420"), "header style kept (bold + orange fill)")
check(ws["C2"].value == 2000, f"C2 overwritten with number -> {ws['C2'].value!r}")
check(ws["C2"].number_format == "#,##0.00", "C2 keeps its number format")
check(ws["A2"].value == "Ana Müller", "untouched unicode cell")
check(ws["D2"].number_format != "General", "date cell keeps date format")
check(ws["C5"].value == "=SUM(C2:C4)", f"original formula kept -> {ws['C5'].value!r}")
check("E6:F6" in [str(r) for r in ws.merged_cells.ranges], "merged range kept")
check(ws.column_dimensions["A"].width == 28, "column width kept")
check(ws.freeze_panes == "A2", "freeze panes kept")
check(len(ws.data_validations.dataValidation) == 1, "data validation kept")
check(ws["A8"].value == "Zoë & <Co>", f"A8 special chars -> {ws['A8']!r}")
check(ws["C8"].value == 42.5 and isinstance(ws["C8"].value, float), "C8 numeric 42.50")
check(ws["D8"].value == "00123", f"D8 leading zeros stay text -> {ws['D8'].value!r}")
check(ws["E8"].value == "=C8*2", f"E8 formula -> {ws['E8'].value!r}")
check(ws["A9"].value == "Header-bold" and ws["A9"].font.bold, "A9 bold via added style")
check(ws["A9"].font.name == ws["A2"].font.name or ws["A9"].font.name, "bold font derived from default font")
check(ws["E3"].value is None, "E3 cleared")
ws2 = wb["Data & Notes"]
check(ws2["A2"].value == "007" and ws2["B2"].value is True, "second sheet untouched")
check(ws2["B5"].value == "new row in sheet 2", "second sheet new cell")
ws3 = wb["Results_ 2026"]
check(ws3["A1"].value == "Title" and ws3["A1"].font.bold, "new sheet created (name sanitised) with bold header")
check(ws3["B2"].value == "x", "new sheet cell")
z = zipfile.ZipFile(f"{out}/patched.xlsx")
check(z.testzip() is None, "zip CRCs valid")
check("xl/calcChain.xml" not in z.namelist(), "stale calcChain dropped")
wbxml = z.read("xl/workbook.xml").decode()
check("fullCalcOnLoad" in wbxml, "workbook set to recalc on load (formulas)")

print("new.xlsx (created from scratch, then re-opened and appended)")
wb = load_workbook(f"{out}/new.xlsx")
check(wb.sheetnames == ["Products", "Second"], f"sheet names {wb.sheetnames}")
ws = wb["Products"]
check([c.value for c in ws[1]] == ["Product", "Price", "URL"] and ws["A1"].font.bold, "bold header row")
check(ws["A2"].value == 'Widget "Pro"' and ws["B2"].value == 19.99, "text + number")
check(ws["C2"].value == "https://example.com/p?a=1&b=2", "URL with & kept")
check(ws["B3"].value == "=B2*3", "formula stored")
check(ws["A4"].value == "Appended", "append after re-open")
check(ws.column_dimensions["C"].width >= 30, f"autofit width {ws.column_dimensions['C'].width}")
check(ws.dimensions == "A1:C4", f"dimension {ws.dimensions}")

print("\nALL OK" if ok else "\nFAILURES")
sys.exit(0 if ok else 1)
