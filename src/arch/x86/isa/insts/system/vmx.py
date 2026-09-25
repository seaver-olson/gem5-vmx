# SPDX-License-Identifier: BSD-3-Clause
"""Native VMX operands: precheck -> normal memory uops -> commit/status."""

microcode = ""


def macro(name, body):
    return f"""
def macroop {name} {{
    .serialize_before
    .serialize_after
    .adjust_env maxOsz
{body}
finished:
    vmxflags t1, dataSize=8
}};
"""


def gate():
    # Test only the private microcode zero flag, preserving architectural flags
    # until operand accesses have completed successfully.
    return """
    srli t4, t1, 8, flags=(EZF,), dataSize=8
    br label("finished"), flags=(CEZF,)
"""


for name, operation in (("VMXON", 27), ("VMCLEAR", 19),
                        ("VMPTRLD", 21), ("VMPTRST", 22)):
    for form, address in (("M", "sib"), ("P", "riprel")):
        body = f"    vmxprep t1, t0, {operation}, dataSize=8\n" + gate()
        if form == "P":
            body += "    rdip t7\n"
        if name == "VMPTRST":
            body += f"    st t2, seg, {address}, disp, dataSize=8, nonSpec=True\n"
        else:
            body += f"    ldvmx t2, seg, {address}, disp, dataSize=8\n"
            if name in ("VMXON", "VMPTRLD"):
                body += f"    vmxregion t1, t2, {operation}, dataSize=8\n" + gate()
                body += "    ldvmxheader t3, ds, [1, t0, t2], dataSize=4, addressSize=8\n"
            else:
                body += "    limm t3, 0, dataSize=8\n"
            body += f"    vmxcommit t1, t0, {operation}, dataSize=8\n"
        microcode += macro(f"{name}_{form}", body)

for form, address in (("R", None), ("M", "sib"), ("P", "riprel")):
    field = "regm" if form == "R" else "reg"
    body = f"    vmxprep t1, {field}, 23, dataSize=8\n" + gate()
    if form == "R":
        body += "    mov reg, reg, t2\n"
    else:
        if form == "P":
            body += "    rdip t7\n"
        body += f"    st t2, seg, {address}, disp, nonSpec=True\n"
    microcode += macro(f"VMREAD_{form}_R", body)

    body = "    vmxprep t1, reg, 25, dataSize=8\n" + gate()
    if form == "R":
        body += "    mov t2, t2, regm\n"
    else:
        if form == "P":
            body += "    rdip t7\n"
        body += f"    ldvmx t2, seg, {address}, disp\n"
    body += "    limm t3, 0, dataSize=8\n    vmxcommit t1, reg, 25, dataSize=8\n"
    microcode += macro(f"VMWRITE_R_{form}", body)
