#!/usr/bin/env python3
"""Creates .xlsx fixtures with openpyxl: styles, formulas, dates, unicode, merged cells, 2 sheets."""
import datetime
import sys

from openpyxl import Workbook
from openpyxl.styles import Font, PatternFill
from openpyxl.worksheet.datavalidation import DataValidation

out = sys.argv[1]

wb = Workbook()
ws = wb.active
ws.title = "Customers"
ws.append(["Name", "Email", "Amount", "Joined", "Notes"])
for c in ws[1]:
    c.font = Font(bold=True, color="FFFFFF")
    c.fill = PatternFill("solid", fgColor="E95420")
ws.append(["Ana Müller", "ana@example.com", 1250.5, datetime.date(2024, 3, 15), "ünïcode & <tags> \"quoted\""])
ws.append(["Bob O'Neil", "bob@example.com", 99, datetime.datetime(2025, 1, 2, 13, 30, 0), "line1\nline2"])
ws.append(["日本語の名前", "jp@example.com", 0.1 + 0.2, None, None])
ws.append(["Total", None, "=SUM(C2:C4)", None, None])
ws["C2"].number_format = "#,##0.00"
ws.merge_cells("E6:F6")
ws["E6"] = "merged"
ws.column_dimensions["A"].width = 28
ws.freeze_panes = "A2"
dv = DataValidation(type="list", formula1='"Yes,No"', allow_blank=True)
ws.add_data_validation(dv)
dv.add("E2:E4")

ws2 = wb.create_sheet("Data & Notes")
ws2["A1"] = "Phone"
ws2["A2"] = "007"          # leading zeros as text
ws2["B2"] = True
ws2["C2"] = "=A2&\"x\""
wb.save(out)
print("wrote", out)
