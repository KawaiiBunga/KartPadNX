#!/usr/bin/env python3
"""Replay the b73 translator change on a staged WiiCompiled tree: post-call reload elision for
nonvolatile GPRs (docs/TRANSLATOR-PLAN-B73.md W1/W2, docs/PERF-B73-2026-10-14.md).

    python builder/apply-translator-reload-elision.py build/wiicompiled-fpscr

The staged translator is gitignored, so this script plus switch/translator/ is the durable record:
  - Translator.Core/Analysis/GuestNonvolatilePreservationAnalyzer.cs (new, copied)
  - Translator.Tests/GuestNonvolatilePreservationAnalyzerTests.cs (new, copied)
  - anchored edits in RegisterResidency, EmitInstruction, the residency codegen tests and Program.cs
Idempotent: an edit already present is skipped. Translating with KARTPAD_RELOAD_ELISION=0 reproduces
the pre-b73 output.
"""
from __future__ import annotations

import pathlib
import shutil
import sys

HERE = pathlib.Path(__file__).resolve().parent
TRACKED = HERE.parent / "switch" / "translator"


def patch_file(path: pathlib.Path, needle: str, replacement: str, marker: str) -> None:
    raw = path.read_bytes()
    bom = raw.startswith(b"\xef\xbb\xbf")
    text = raw.decode("utf-8-sig")
    newline = "\r\n" if "\r\n" in text else "\n"
    if marker in text:
        return
    needle = needle.replace("\n", newline)
    replacement = replacement.replace("\n", newline)
    if text.count(needle) != 1:
        raise RuntimeError(f"expected one replay anchor in {path}, found {text.count(needle)}")
    text = text.replace(needle, replacement)
    path.write_bytes((b"\xef\xbb\xbf" if bom else b"") + text.encode("utf-8"))


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__, file=sys.stderr)
        return 64
    root = pathlib.Path(sys.argv[1]).resolve()
    core = root / "translator/src/Translator.Core"
    tests = root / "translator/tests/Translator.Tests"
    shutil.copyfile(TRACKED / "GuestNonvolatilePreservationAnalyzer.cs",
                    core / "Analysis/GuestNonvolatilePreservationAnalyzer.cs")
    shutil.copyfile(TRACKED / "GuestNonvolatilePreservationAnalyzerTests.cs",
                    tests / "GuestNonvolatilePreservationAnalyzerTests.cs")

    residency = core / "CodeGen/CxxLinearCodeGenerator.RegisterResidency.cs"
    patch_file(residency,
               """            foreach (var gpr in _gprs)
                if (sync.ReloadsGpr(gpr))
                    sb.Append(pad).Append(GprLocal(gpr)).Append(" = ctx->gpr[").Append(gpr).AppendLine("];");""",
               """            foreach (var gpr in _gprs)
                if (sync.ReloadsGpr(gpr))
                {
                    // A callee proven (or, for indirect calls, assumed by the EABI) to preserve the
                    // register leaves ctx equal to the local. The blank line keeps every later line
                    // number of the body unchanged, so the existing sample PGO profile still maps.
                    if ((sync.ExemptReloadGprMask & (1u << gpr)) != 0)
                        sb.AppendLine();
                    else
                        sb.Append(pad).Append(GprLocal(gpr)).Append(" = ctx->gpr[").Append(gpr).AppendLine("];");
                }""",
               "ExemptReloadGprMask & (1u << gpr)")
    patch_file(residency,
               """        public bool FlushesGpr(int index) => (FlushGprMask & (1u << index)) != 0;""",
               """        /// <summary>
        /// GPRs whose reload is skipped although <see cref="ReloadGprMask"/> names them: the callee returns
        /// them unchanged (GuestNonvolatilePreservationAnalyzer). Flushes are unaffected.
        /// </summary>
        public uint ExemptReloadGprMask { get; init; }

        public bool FlushesGpr(int index) => (FlushGprMask & (1u << index)) != 0;""",
               "public uint ExemptReloadGprMask { get; init; }")

    emit = core / "CodeGen/CxxLinearCodeGenerator.EmitInstruction.cs"
    patch_file(emit,
               """                    EmitNormalizeGuestCallFloatReturn(call.Target, sb, pad, localPaired, guestAbiProvider);
                    AppendReload(sb, pad, boundarySync);
                }""",
               """                    EmitNormalizeGuestCallFloatReturn(call.Target, sb, pad, localPaired, guestAbiProvider);
                    // State-free arms and float-argument normalization write CpuContext themselves, so
                    // only plain InvokeDirectCpu sites drop reloads of callee-preserved GPRs.
                    var reloadSync = usesStateFreeCall || argumentNormalization.Length != 0
                        ? boundarySync
                        : boundarySync with
                        {
                            ExemptReloadGprMask = GuestNonvolatilePreservation.DirectCallExemption(addr)
                        };
                    AppendReload(sb, pad, reloadSync);
                }""",
               "GuestNonvolatilePreservation.DirectCallExemption(addr)")
    patch_file(emit,
               """                    AppendFlush(sb, pad);
                    sb.AppendLine($"{pad}InvokeIndirectCpu({indirectTarget}, ctx);");
                    AppendReload(sb, pad);""",
               """                    AppendFlush(sb, pad);
                    sb.AppendLine($"{pad}InvokeIndirectCpu({indirectTarget}, ctx);");
                    // EABI: the callee returns r1, r2 and r13-r31 unchanged (runtime check: perf.cfg
                    // eabi_verify). Every other register is reloaded as before.
                    AppendReload(sb, pad, ResidencyBoundarySync.Full with
                    {
                        ExemptReloadGprMask = GuestNonvolatilePreservation.IndirectCallExemption
                    });""",
               "GuestNonvolatilePreservation.IndirectCallExemption")

    program = root / "translator/src/Translator.Cli/Program.cs"
    recursive_block = (TRACKED / "program-block-recursive.cs.txt").read_text(encoding="utf-8")
    patch_file(program,
               """            var directlyCalledGuestFunctions = localGuestAbiContracts.Values""",
               recursive_block + """            var directlyCalledGuestFunctions = localGuestAbiContracts.Values""",
               "GuestNonvolatilePreservationAnalyzer.Analyze(")
    mod_block = (TRACKED / "program-block-mod.cs.txt").read_text(encoding="utf-8")
    patch_file(program,
               """    // The base translation bakes mod-safety decisions""",
               mod_block + """    // The base translation bakes mod-safety decisions""",
               "Nonvolatile reload exemptions loaded")

    residency_tests = tests / "RegisterResidencyCodeGenTests.cs"
    patch_file(residency_tests,
               """        // There is no callee contract to narrow against, so the boundary stays
        // full: r4 is reloaded after the call even though this frame only reads it.
        Assert.Contains("    r4 = ctx->gpr[4];", narrowed, StringComparison.Ordinal);
    }""",
               """        // There is no callee contract to narrow against, so the boundary stays
        // full: r4 is reloaded after the call even though this frame only reads it.
        Assert.Contains("    r4 = ctx->gpr[4];", narrowed, StringComparison.Ordinal);
    }

    [Fact]
    public void IndirectCallKeepsEabiNonvolatileLocalsAsBlankLines()
    {
        var function = new IrFunction("eabi_indirect", "entry", new[]
        {
            new IrBasicBlock("entry", new IrInstruction[]
            {
                new IrBinary("r31", IrValue.Register("r31"), IrValue.Register("r4"), "add"),
                new IrIndirectCall(string.Empty, IrValue.Register("r12"), Array.Empty<IrValue>()),
                new IrBinary("r3", IrValue.Register("r31"), IrValue.Register("r4"), "add"),
                new IrReturn(null)
            })
        });

        var body = FunctionBody(Emit(function), "eabi_indirect");
        var call = body.IndexOf("InvokeIndirectCpu(", StringComparison.Ordinal);

        // r31 is still published for the callee, never reloaded; volatile r4 is reloaded as before.
        Assert.Contains("ctx->gpr[31] = r31;", body[..call], StringComparison.Ordinal);
        Assert.DoesNotContain("r31 = ctx->gpr[31];", body[call..], StringComparison.Ordinal);
        Assert.Contains("    r4 = ctx->gpr[4];", body[call..], StringComparison.Ordinal);
    }""",
               "IndirectCallKeepsEabiNonvolatileLocalsAsBlankLines")
    print("translator reload elision: applied")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
