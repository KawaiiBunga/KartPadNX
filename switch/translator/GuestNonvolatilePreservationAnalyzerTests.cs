using System;
using System.Collections.Generic;
using Translator.Core.Analysis;
using Translator.Core.Ir;
using Xunit;

namespace Translator.Tests;

public sealed class GuestNonvolatilePreservationAnalyzerTests
{
    private const uint R1 = 1u << 1;
    private const uint R30 = 1u << 30;
    private const uint R31 = 1u << 31;

    private static GuestPreservationCallPolicy Policy(
        IReadOnlySet<uint>? modOverridable = null,
        IReadOnlyDictionary<uint, GuestAbiContract>? contracts = null) =>
        new(contracts ?? new Dictionary<uint, GuestAbiContract>(), new HashSet<uint>(),
            modOverridable ?? new HashSet<uint>(), new HashSet<uint>(), new HashSet<uint>());

    private static IrFunction Function(string name, params IrInstruction[] instructions) =>
        new(name, "entry", new[] { new IrBasicBlock("entry", instructions) });

    private static IrInstruction[] Prologue(int frame, params string[] saved)
    {
        var list = new List<IrInstruction>
        {
            new IrStore(new IrAddress("r1", -frame), IrValue.Register("r1"), 4),
            new IrBinary("r1", IrValue.Register("r1"), IrValue.Imm(-frame), "add")
        };
        for (var index = 0; index < saved.Length; ++index)
            list.Add(new IrStore(new IrAddress("r1", frame - 4 - index * 4), IrValue.Register(saved[index]), 4));
        return list.ToArray();
    }

    private static IrInstruction[] Epilogue(int frame, params string[] saved)
    {
        var list = new List<IrInstruction>();
        for (var index = 0; index < saved.Length; ++index)
            list.Add(new IrLoad(saved[index], new IrAddress("r1", frame - 4 - index * 4), 4));
        list.Add(new IrBinary("r1", IrValue.Register("r1"), IrValue.Imm(frame), "add"));
        return list.ToArray();
    }

    private static IrInstruction[] Concat(params IrInstruction[][] parts)
    {
        var list = new List<IrInstruction>();
        foreach (var part in parts) list.AddRange(part);
        return list.ToArray();
    }

    [Fact]
    public void SavedAndRestoredRegisterIsPreservedAcrossAnUnknownCall()
    {
        var function = Function("f", Concat(
            Prologue(16, "r31"),
            new IrInstruction[]
            {
                new IrAssign("r31", IrValue.Imm(5)),
                new IrCall("lr", "0x80009000", Array.Empty<IrValue>())
            },
            Epilogue(16, "r31"),
            new IrInstruction[] { new IrReturn(null) }));

        var masks = GuestNonvolatilePreservationAnalyzer.Analyze(
            new Dictionary<uint, IrFunction> { [0x80001000u] = function }, Policy());

        Assert.NotEqual(0u, masks[0x80001000u] & R1);
        Assert.NotEqual(0u, masks[0x80001000u] & R31);
        // r30 was neither saved nor kept: the unknown callee may have changed it.
        Assert.Equal(0u, masks[0x80001000u] & R30);
    }

    [Fact]
    public void OverwrittenRegisterIsNotPreserved()
    {
        var function = Function("f", new IrAssign("r31", IrValue.Imm(1)), new IrReturn(null));
        var masks = GuestNonvolatilePreservationAnalyzer.Analyze(
            new Dictionary<uint, IrFunction> { [0x80001000u] = function }, Policy());
        Assert.Equal(0u, masks[0x80001000u] & R31);
        Assert.NotEqual(0u, masks[0x80001000u] & R1);
    }

    [Fact]
    public void StoreOverTheSaveSlotBreaksTheProof()
    {
        var function = Function("f", Concat(
            Prologue(16, "r31"),
            new IrInstruction[]
            {
                new IrAssign("r31", IrValue.Imm(5)),
                new IrStore(new IrAddress("r1", 12), IrValue.Register("r3"), 4)
            },
            Epilogue(16, "r31"),
            new IrInstruction[] { new IrReturn(null) }));
        var masks = GuestNonvolatilePreservationAnalyzer.Analyze(
            new Dictionary<uint, IrFunction> { [0x80001000u] = function }, Policy());
        Assert.Equal(0u, masks[0x80001000u] & R31);
    }

    [Fact]
    public void IndirectCallIsAssumedToFollowTheEabi()
    {
        var function = Function("f",
            new IrIndirectCall("lr", IrValue.Register("ctr"), Array.Empty<IrValue>()),
            new IrReturn(null));
        var masks = GuestNonvolatilePreservationAnalyzer.Analyze(
            new Dictionary<uint, IrFunction> { [0x80001000u] = function }, Policy());
        Assert.Equal(GuestNonvolatilePreservationAnalyzer.TrackedGprMask, masks[0x80001000u]);
    }

    [Fact]
    public void RecursionIsProvenByInduction()
    {
        var function = Function("f", Concat(
            Prologue(16, "r31"),
            new IrInstruction[]
            {
                new IrAssign("r31", IrValue.Register("r3")),
                new IrCall("lr", "0x80001000", Array.Empty<IrValue>())
            },
            Epilogue(16, "r31"),
            new IrInstruction[] { new IrReturn(null) }));
        var masks = GuestNonvolatilePreservationAnalyzer.Analyze(
            new Dictionary<uint, IrFunction> { [0x80001000u] = function }, Policy());
        Assert.Equal(GuestNonvolatilePreservationAnalyzer.TrackedGprMask, masks[0x80001000u]);
    }

    [Fact]
    public void SharedEpilogueFragmentIsSummarizedForItsCaller()
    {
        // Fragment: reloads r31 from the caller's frame and pops it.
        var fragment = Function("fragment", Concat(
            Epilogue(16, "r31"),
            new IrInstruction[] { new IrReturn(null) }));
        var caller = Function("caller", Concat(
            Prologue(16, "r31"),
            new IrInstruction[]
            {
                new IrAssign("r31", IrValue.Imm(7)),
                new IrCall("lr", "0x80002000", Array.Empty<IrValue>()),
                new IrReturn(null)
            }));
        var masks = GuestNonvolatilePreservationAnalyzer.Analyze(
            new Dictionary<uint, IrFunction> { [0x80001000u] = caller, [0x80002000u] = fragment }, Policy());

        Assert.Equal(0u, masks[0x80002000u] & (R1 | R31));
        Assert.Equal(GuestNonvolatilePreservationAnalyzer.TrackedGprMask, masks[0x80001000u]);
    }

    [Fact]
    public void ModOverridableCalleeKeepsOnlyStackDisciplineAndTheCallersSaveSlots()
    {
        var function = Function("f", Concat(
            Prologue(16, "r31"),
            new IrInstruction[] { new IrCall("lr", "0x80003000", Array.Empty<IrValue>()) },
            Epilogue(16, "r31"),
            new IrInstruction[] { new IrReturn(null) }));
        var callee = Function("callee", new IrReturn(null));
        var masks = GuestNonvolatilePreservationAnalyzer.Analyze(
            new Dictionary<uint, IrFunction> { [0x80001000u] = function, [0x80003000u] = callee },
            Policy(modOverridable: new HashSet<uint> { 0x80003000u }));

        Assert.NotEqual(0u, masks[0x80001000u] & R31);
        Assert.Equal(0u, masks[0x80001000u] & R30);
        Assert.Equal(GuestNonvolatilePreservationAnalyzer.StackDisciplineMask, GuestNonvolatilePreservationAnalyzer.CalleePreservedGprs(
            0x80003000u, masks, Policy(modOverridable: new HashSet<uint> { 0x80003000u })));
    }

    [Fact]
    public void IndirectTailJumpIsAnEabiExit()
    {
        var function = Function("f",
            new IrAssign("r12", IrValue.Register("r3")),
            new IrIndirectJump(IrValue.Register("r12")),
            new IrReturn(null));
        var masks = GuestNonvolatilePreservationAnalyzer.Analyze(
            new Dictionary<uint, IrFunction> { [0x80001000u] = function }, Policy());
        Assert.Equal(GuestNonvolatilePreservationAnalyzer.TrackedGprMask, masks[0x80001000u]);
    }

    [Fact]
    public void FallThroughBlocksAreFollowed()
    {
        var function = new IrFunction("f", "entry", new[]
        {
            new IrBasicBlock("entry", new IrInstruction[] { new IrSetCrField(0, IrValue.Register("r3"), IrValue.Imm(0), false) }),
            new IrBasicBlock("next", new IrInstruction[] { new IrAssign("r31", IrValue.Imm(1)) }),
            new IrBasicBlock("exit", new IrInstruction[] { new IrReturn(null) })
        });
        var masks = GuestNonvolatilePreservationAnalyzer.Analyze(
            new Dictionary<uint, IrFunction> { [0x80001000u] = function }, Policy());
        Assert.Equal(0u, masks[0x80001000u] & R31);
        Assert.NotEqual(0u, masks[0x80001000u] & R1);
    }
}
