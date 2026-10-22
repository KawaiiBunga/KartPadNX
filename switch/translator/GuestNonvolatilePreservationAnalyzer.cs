using System;
using System.Collections.Generic;
using System.Linq;
using System.Threading.Tasks;
using Translator.Core.Ir;

namespace Translator.Core.Analysis;

/// <summary>
/// Inputs that decide how a call inside an analyzed body affects the nonvolatile GPRs.
/// </summary>
public sealed record GuestPreservationCallPolicy(
    IReadOnlyDictionary<uint, GuestAbiContract> Contracts,
    IReadOnlySet<uint> NativeTargets,
    IReadOnlySet<uint> ModOverridableTargets,
    IReadOnlySet<uint> NonReturningTargets,
    IReadOnlySet<uint> LrContinuationTargets)
{
    /// <summary>
    /// Natives that change a caller's nonvolatile GPRs on purpose: OSLoadContext (0x801A1F58, the one
    /// native the runtime never guards) and NAND's IOS_Open body entry (0x801938FC, pops a frame off r1).
    /// Found by scanning runtime/src for writes to gpr[1], gpr[2] and gpr[13..31] (b73).
    /// </summary>
    public static IReadOnlySet<uint> DefaultNonEabiNatives { get; } = new HashSet<uint> { 0x801A1F58u, 0x801938FCu };

    public IReadOnlySet<uint> NonEabiNativeTargets { get; init; } = DefaultNonEabiNatives;
}

/// <summary>
/// Proves which nonvolatile GPRs (r1, r2 and r13-r31) a guest function returns holding their entry values,
/// so a caller's post-call reload of those registers is redundant (b73, docs/TRANSLATOR-PLAN-B73.md W1/W2).
///
/// A forward dataflow over each body tracks every GPR as one of: the entry value of some register (r1 also
/// as entry r1 plus a constant), the word the caller's frame held at entry r1 + offset, or unknown; plus the
/// r1-relative stack slots holding such values. That covers the CodeWarrior prologue/epilogue forms
/// (stw/lwz, stmw/lmw, the _save_gpr/_rest_gpr thunks) and, through per-function exit summaries, shared
/// epilogue fragments that pop the caller's frame and reload its registers.
///
/// An indirect call is assumed to follow the EABI and preserve these registers (W1, checked at runtime by
/// perf.cfg eabi_verify). Natives are EABI (the runtime restores r14-r31 around every native except
/// OSLoadContext) except <see cref="GuestPreservationCallPolicy.NonEabiNativeTargets"/>. Everything else the
/// analysis cannot model ends the proof: mod-overridable, LR-continuation and non-returning callees,
/// indirect jumps, undefined instructions, falling off the last block. Summaries start from "nothing
/// known" and every round uses only facts already proven (least fixed point), so recursion proves nothing.
/// </summary>
public static class GuestNonvolatilePreservationAnalyzer
{
    /// <summary>r1, r2 and r13-r31.</summary>
    public const uint TrackedGprMask = (1u << 1) | (1u << 2) | 0xFFFFE000u;

    /// <summary>GPRs an EABI callee returns unchanged; assumed at indirect calls.</summary>
    public const uint EabiPreservedGprMask = TrackedGprMask;

    /// <summary>
    /// GPRs every callee that returns to compiled code leaves alone, ABI or not: r1 (the caller's epilogue
    /// reads its saved registers and LR through it) and the SDA bases r2/r13.
    /// </summary>
    public const uint StackDisciplineMask = (1u << 1) | (1u << 2) | (1u << 13);

    private const GuestCallBoundaryFlags OpaqueHelperFlags =
        GuestCallBoundaryFlags.RequiresCompleteContext | GuestCallBoundaryFlags.CanSuspend |
        GuestCallBoundaryFlags.CanSwitchThreads | GuestCallBoundaryFlags.InvokesGuestCode;

    private const int MaxRounds = 256;
    private const int MaxCallerWrites = 32;

    /// <summary>
    /// A value relative to the analyzed function's entry. Kind 1: entry value of <see cref="Register"/> plus
    /// <see cref="Delta"/> (non-zero only for r1). Kind 2: the word at entry r1 + <see cref="Delta"/> in the
    /// caller's frame, as it was at entry.
    /// </summary>
    private readonly record struct Value(byte Kind, byte Register, int Delta)
    {
        public static Value Unknown => default;
        public static Value Entry(int register, int delta = 0) => new(1, (byte)register, delta);
        public static Value CallerSlot(int offset) => new(2, 0, offset);
        public bool IsStack => Kind == 1 && Register == 1;
    }

    private readonly record struct Slot(int Size, Value Value);

    /// <summary>
    /// A function's effect as its caller sees it: each GPR's exit value, and the caller-frame words it may
    /// overwrite (offsets from its entry r1), or <see cref="ClobbersCallerFrame"/> when that is unknown.
    /// </summary>
    private sealed record Summary(Value[] Exit, (int Offset, int Size)[] CallerWrites, bool ClobbersCallerFrame)
    {
        /// <summary>The optimistic start of the greatest fixed point: imposes nothing in a meet.</summary>
        public bool IsTop { get; init; }

        /// <summary>
        /// Nothing known about the callee beyond stack discipline: it returns r1 as it found it (the
        /// caller's epilogue finds its saved registers and LR through r1, so a callee that moved r1 would
        /// crash the original game too), leaves the SDA bases r2/r13 alone, and stores only to its caller's
        /// linkage words, never into the caller's save area.
        /// </summary>
        public static Summary Bottom { get; } = new(
            Enumerable.Range(0, 32).Select(static register =>
                (StackDisciplineMask & (1u << register)) != 0 ? Value.Entry(register) : Value.Unknown).ToArray(),
            new[] { (0, 8) }, false);

        /// <summary>Not even stack discipline (LR-continuation targets, unanalyzable bodies).</summary>
        public static Summary Opaque { get; } = new(new Value[32], new[] { (0, 8) }, true);

        /// <summary>Everything preserved, nothing stored: the start of the greatest-fixed-point iteration.</summary>
        public static Summary Top { get; } = new(
            Enumerable.Range(0, 32).Select(static register => Value.Entry(register)).ToArray(),
            Array.Empty<(int, int)>(), false) { IsTop = true };

        /// <summary>Facts both summaries agree on; stores accumulate.</summary>
        public Summary Meet(Summary other)
        {
            if (other.IsTop) return this;
            if (IsTop) return other;
            var exit = new Value[32];
            for (var register = 0; register < 32; ++register)
                exit[register] = Exit[register] == other.Exit[register] ? Exit[register] : Value.Unknown;
            var writes = CallerWrites.Concat(other.CallerWrites).Distinct()
                .OrderBy(static w => w.Offset).ThenBy(static w => w.Size).ToArray();
            var clobbers = ClobbersCallerFrame || other.ClobbersCallerFrame || writes.Length > MaxCallerWrites;
            return new Summary(exit, clobbers ? Array.Empty<(int, int)>() : writes, clobbers);
        }

        public uint PreservedMask
        {
            get
            {
                var mask = 0u;
                for (var register = 0; register < 32; ++register)
                    if (Exit[register] == Value.Entry(register)) mask |= 1u << register;
                return mask & TrackedGprMask;
            }
        }

        public bool SameAs(Summary other) =>
            IsTop == other.IsTop &&
            ClobbersCallerFrame == other.ClobbersCallerFrame && Exit.AsSpan().SequenceEqual(other.Exit) &&
            CallerWrites.AsSpan().SequenceEqual(other.CallerWrites);
    }

    /// <summary>Preserved-GPR masks for every function in <paramref name="functions"/>.</summary>
    public static IReadOnlyDictionary<uint, uint> Analyze(
        IReadOnlyDictionary<uint, IrFunction> functions,
        GuestPreservationCallPolicy policy)
    {
        var summaries = AnalyzeSummaries(functions, policy);
        return summaries.ToDictionary(static pair => pair.Key, static pair => pair.Value.PreservedMask);
    }

    /// <summary>
    /// Optimistic iteration: every summary starts at <see cref="Summary.Top"/> (everything preserved) and
    /// each round recomputes every body under the previous round's summaries until nothing changes. Any
    /// fixed point is sound: each summary then holds for its body given that all callees, recursive ones
    /// included, satisfy theirs, which by induction on the call depth holds for every call that returns
    /// (a recursive heap search such as MEM::FindContainHeap is provable this way; iterating up from
    /// "nothing known" proves nothing for it). Intermediate rounds are not sound, so if the iteration does
    /// not settle within <see cref="MaxRounds"/> the result comes from iterating up from
    /// <see cref="Summary.Bottom"/> instead, which is sound at every round.
    /// </summary>
    private static Dictionary<uint, Summary> AnalyzeSummaries(
        IReadOnlyDictionary<uint, IrFunction> functions,
        GuestPreservationCallPolicy policy)
    {
        var addresses = functions.Keys.OrderBy(static address => address).ToArray();
        var bodies = addresses.Select(address => functions[address]).ToArray();
        var greatest = Iterate(addresses, bodies, policy, Summary.Top, meetWithPrevious: false, out var converged);
        if (converged) return greatest;
        Console.WriteLine("[translator] Nonvolatile GPR preservation: greatest fixed point did not converge; " +
                          "using the least fixed point.");
        return Iterate(addresses, bodies, policy, Summary.Bottom, meetWithPrevious: false, out _);
    }

    private static Dictionary<uint, Summary> Iterate(
        uint[] addresses,
        IrFunction[] bodies,
        GuestPreservationCallPolicy policy,
        Summary start,
        bool meetWithPrevious,
        out bool converged)
    {
        var current = new Dictionary<uint, Summary>(addresses.Length);
        foreach (var address in addresses) current[address] = start;
        var parallel = new ParallelOptions { MaxDegreeOfParallelism = Environment.ProcessorCount };
        var next = new Summary[addresses.Length];
        for (var round = 0; round < MaxRounds; ++round)
        {
            var snapshot = new Dictionary<uint, Summary>(current);
            Parallel.For(0, addresses.Length, parallel, index =>
            {
                var computed = SummarizeOrBottom(bodies[index], snapshot, policy);
                next[index] = meetWithPrevious ? computed.Meet(snapshot[addresses[index]]) : computed;
            });
            var changed = false;
            for (var index = 0; index < addresses.Length; ++index)
            {
                if (next[index].SameAs(current[addresses[index]])) continue;
                current[addresses[index]] = next[index];
                changed = true;
            }
            if (!changed)
            {
                converged = true;
                return current;
            }
        }
        converged = false;
        return current;
    }

    /// <summary>
    /// GPRs a call to <paramref name="target"/> leaves unchanged, given proven masks for translated
    /// callees. Used for the code generator's reload exemptions.
    /// </summary>
    public static uint CalleePreservedGprs(
        uint target,
        IReadOnlyDictionary<uint, uint> preserved,
        GuestPreservationCallPolicy policy)
    {
        if (policy.NonReturningTargets.Contains(target) || policy.LrContinuationTargets.Contains(target))
            return 0;
        if (policy.ModOverridableTargets.Contains(target)) return StackDisciplineMask;
        policy.Contracts.TryGetValue(target, out var contract);
        var notWritten = contract is not null && !contract.HasFullSynchronizationFence
            ? ~contract.GprPossibleWriteMask & TrackedGprMask
            : 0u;
        if (policy.NativeTargets.Contains(target))
            return policy.NonEabiNativeTargets.Contains(target) ? notWritten : TrackedGprMask;
        return (preserved.GetValueOrDefault(target) | notWritten | StackDisciplineMask) & TrackedGprMask;
    }

    /// <summary>The caller-visible effect of a call to <paramref name="target"/>.</summary>
    private static Summary CalleeSummary(
        uint target,
        IReadOnlyDictionary<uint, Summary> summaries,
        GuestPreservationCallPolicy policy)
    {
        if (policy.LrContinuationTargets.Contains(target)) return Summary.Opaque;
        if (policy.NonReturningTargets.Contains(target) || policy.ModOverridableTargets.Contains(target))
            return Summary.Bottom;
        policy.Contracts.TryGetValue(target, out var contract);
        var notWritten = contract is not null && !contract.HasFullSynchronizationFence
            ? ~contract.GprPossibleWriteMask
            : 0u;
        Value[] exit;
        (int, int)[] callerWrites;
        bool clobbers;
        if (policy.NativeTargets.Contains(target))
        {
            if (!policy.NonEabiNativeTargets.Contains(target)) notWritten |= TrackedGprMask;
            exit = new Value[32];
            // A native may still store to its caller's linkage words like any EABI function.
            callerWrites = new[] { (0, 8) };
            clobbers = false;
        }
        else if (summaries.TryGetValue(target, out var summary))
        {
            exit = (Value[])summary.Exit.Clone();
            callerWrites = summary.CallerWrites;
            clobbers = summary.ClobbersCallerFrame;
        }
        else
        {
            return Summary.Bottom;
        }
        for (var register = 0; register < 32; ++register)
            if ((notWritten & (1u << register)) != 0) exit[register] = Value.Entry(register);
        return new Summary(exit, callerWrites, clobbers);
    }

    private sealed class State
    {
        public readonly Value[] Gpr = new Value[32];
        public readonly Dictionary<string, Value> Temps = new(StringComparer.Ordinal);
        public readonly Dictionary<int, Slot> Slots = new();
        /// <summary>Caller-frame bytes (offset >= 0 from entry r1) this body stored to on some path.</summary>
        public readonly HashSet<(int Offset, int Size)> Touched = new();
        public bool AllTouched;

        public State Clone()
        {
            var copy = new State();
            Array.Copy(Gpr, copy.Gpr, 32);
            foreach (var pair in Temps) copy.Temps[pair.Key] = pair.Value;
            foreach (var pair in Slots) copy.Slots[pair.Key] = pair.Value;
            copy.Touched.UnionWith(Touched);
            copy.AllTouched = AllTouched;
            return copy;
        }

        public bool SameAs(State other)
        {
            for (var index = 0; index < 32; ++index)
                if (Gpr[index] != other.Gpr[index]) return false;
            if (AllTouched != other.AllTouched || !Touched.SetEquals(other.Touched)) return false;
            if (Temps.Count != other.Temps.Count || Slots.Count != other.Slots.Count) return false;
            foreach (var pair in Temps)
                if (!other.Temps.TryGetValue(pair.Key, out var value) || value != pair.Value) return false;
            foreach (var pair in Slots)
                if (!other.Slots.TryGetValue(pair.Key, out var slot) || slot != pair.Value) return false;
            return true;
        }

        /// <summary>Meet: a value fact survives only when every predecessor agrees; stores accumulate.</summary>
        public void MeetWith(State other)
        {
            for (var index = 0; index < 32; ++index)
                if (Gpr[index] != other.Gpr[index]) Gpr[index] = Value.Unknown;
            foreach (var key in Temps.Keys.ToArray())
                if (!other.Temps.TryGetValue(key, out var value) || value != Temps[key]) Temps.Remove(key);
            foreach (var key in Slots.Keys.ToArray())
                if (!other.Slots.TryGetValue(key, out var slot) || slot != Slots[key]) Slots.Remove(key);
            Touched.UnionWith(other.Touched);
            AllTouched |= other.AllTouched;
        }

        public void Kill(int offset, int size)
        {
            if (offset + size > 0) Touched.Add((offset, size));
            if (Slots.Count == 0) return;
            foreach (var key in Slots.Keys.ToArray())
            {
                var slot = Slots[key];
                if (key < offset + size && offset < key + slot.Size) Slots.Remove(key);
            }
        }

        public bool CallerWordTouched(int offset)
        {
            if (AllTouched) return true;
            foreach (var (start, size) in Touched)
                if (start < offset + 4 && offset < start + size) return true;
            return false;
        }
    }

    /// <summary>Path-independent facts gathered while analyzing one body.</summary>
    private sealed class Effects
    {
        public readonly HashSet<(int Offset, int Size)> CallerWrites = new();
        public bool ClobbersCallerFrame;

        public void NoteStore(int offset, int size)
        {
            if (offset + size <= 0) return;  // the function's own frame, below its entry r1
            if (CallerWrites.Count < MaxCallerWrites) CallerWrites.Add((offset, size));
            else ClobbersCallerFrame = true;
        }
    }

    private sealed class Unanalyzable : Exception
    {
        public Unanalyzable(string reason = "") : base(reason) { }
    }

    private static Summary SummarizeOrBottom(
        IrFunction function,
        IReadOnlyDictionary<uint, Summary> summaries,
        GuestPreservationCallPolicy policy)
    {
        try
        {
            return Summarize(function, summaries, policy);
        }
        catch (Unanalyzable)
        {
            return Summary.Bottom;
        }
        catch (OverflowException)
        {
            return Summary.Bottom;
        }
    }

    private static Summary Summarize(
        IrFunction function,
        IReadOnlyDictionary<uint, Summary> summaries,
        GuestPreservationCallPolicy policy)
    {
        var blocks = function.Blocks;
        var ordinals = new Dictionary<string, int>(blocks.Count, StringComparer.Ordinal);
        for (var index = 0; index < blocks.Count; ++index) ordinals[blocks[index].Label] = index;
        if (!ordinals.TryGetValue(function.EntryLabel, out var entry)) throw new Unanalyzable("entry label");

        var successors = new int[blocks.Count][];
        for (var index = 0; index < blocks.Count; ++index)
            successors[index] = Successors(blocks, index, ordinals, policy);

        var inStates = new State?[blocks.Count];
        var initial = new State();
        for (var register = 0; register < 32; ++register) initial.Gpr[register] = Value.Entry(register);
        inStates[entry] = initial;

        var effects = new Effects();
        var trace = TraceTarget is { } traced && function.Name.Contains(traced, StringComparison.OrdinalIgnoreCase)
            ? new List<string>()
            : null;
        var worklist = new Queue<int>();
        var queued = new bool[blocks.Count];
        worklist.Enqueue(entry);
        queued[entry] = true;
        var outStates = new State?[blocks.Count];
        var iterations = 0;
        while (worklist.Count != 0)
        {
            if (++iterations > 64 * blocks.Count + 1024) throw new Unanalyzable("no fixed point");
            var index = worklist.Dequeue();
            queued[index] = false;
            var state = inStates[index]!.Clone();
            foreach (var instruction in blocks[index].Instructions)
            {
                Transfer(instruction, state, effects, summaries, policy);
                trace?.Add($"{blocks[index].Label}: {instruction} || r1={Show(state.Gpr[1])} r29={Show(state.Gpr[29])} " +
                           $"r30={Show(state.Gpr[30])} r31={Show(state.Gpr[31])} slots=" +
                           string.Join(",", state.Slots.OrderBy(static p => p.Key)
                               .Select(static p => $"{p.Key}:{p.Value.Size}:{Show(p.Value.Value)}")));
            }
            outStates[index] = state;
            foreach (var successor in successors[index])
            {
                var existing = inStates[successor];
                if (existing is null)
                {
                    inStates[successor] = state.Clone();
                }
                else
                {
                    var merged = existing.Clone();
                    merged.MeetWith(state);
                    if (merged.SameAs(existing)) continue;
                    inStates[successor] = merged;
                }
                if (!queued[successor])
                {
                    queued[successor] = true;
                    worklist.Enqueue(successor);
                }
            }
        }

        Value[]? exit = null;
        for (var index = 0; index < blocks.Count; ++index)
        {
            if (outStates[index] is not { } state || !IsExit(blocks[index])) continue;
            if (exit is null)
            {
                exit = (Value[])state.Gpr.Clone();
                continue;
            }
            for (var register = 0; register < 32; ++register)
                if (exit[register] != state.Gpr[register]) exit[register] = Value.Unknown;
        }
        if (trace is not null)
        {
            trace.Add("EXIT " + (exit is null ? "none" : string.Join(" ", exit.Select(Show))));
            System.IO.File.WriteAllLines(TracePath!, trace);
        }
        // No path returns (panic, idle loop): a call to it never comes back, so any summary holds.
        if (exit is null) return Summary.Top;
        var writes = effects.CallerWrites.OrderBy(static w => w.Offset).ThenBy(static w => w.Size).ToArray();
        return new Summary(exit, writes, effects.ClobbersCallerFrame);
    }

    // KARTPAD_PRESERVATION_TRACE=<function name fragment>;<output path> (diagnostics).
    private static readonly string? TraceTarget =
        Environment.GetEnvironmentVariable("KARTPAD_PRESERVATION_TRACE")?.Split(';')[0];
    private static readonly string? TracePath =
        Environment.GetEnvironmentVariable("KARTPAD_PRESERVATION_TRACE")?.Split(';').ElementAtOrDefault(1);

    private static string Show(Value value) => value.Kind switch
    {
        1 => value.Delta == 0 ? $"E{value.Register}" : $"E{value.Register}{value.Delta:+#;-#}",
        2 => $"C{value.Delta}",
        _ => "?"
    };

    private static bool IsExit(IrBasicBlock block) => Terminator(block) is IrReturn or IrIndirectJump;

    /// <summary>Last instruction that is not a trace marker or comment.</summary>
    private static IrInstruction? Terminator(IrBasicBlock block)
    {
        var instructions = block.Instructions;
        for (var index = instructions.Count - 1; index >= 0; --index)
            if (instructions[index] is not (IrTracePpc or IrComment)) return instructions[index];
        return null;
    }

    private static int[] Successors(
        IReadOnlyList<IrBasicBlock> blocks, int index, Dictionary<string, int> ordinals,
        GuestPreservationCallPolicy policy)
    {
        var terminator = Terminator(blocks[index]);
        int Ordinal(string label) =>
            ordinals.TryGetValue(label, out var ordinal) ? ordinal : throw new Unanalyzable("label " + label);
        switch (terminator)
        {
            case IrBranch branch:
                return new[] { Ordinal(branch.TrueLabel), Ordinal(branch.FalseLabel) };
            case IrJump jump:
                return new[] { Ordinal(jump.TargetLabel) };
            case IrJumpTable table:
                return table.Cases.Select(static c => c.TargetLabel).Distinct(StringComparer.Ordinal)
                    .Select(Ordinal).ToArray();
            case IrReturn:
                return Array.Empty<int>();
            case IrCall call when GuestTargetParser.TryParseAddress(call.Target, out var target) &&
                                  policy.NonReturningTargets.Contains(target):
                // Control never comes back: this path ends without reaching an exit.
                return Array.Empty<int>();
            case IrIndirectJump:
                // Tail call through ctr (__ptmf_scall and friends): the target returns to this body's
                // caller, so the jump is an exit; Transfer applied the EABI effect.
                return Array.Empty<int>();
            case IrUndefined:
                throw new Unanalyzable("terminator " + terminator.GetType().Name);
            default:
                // No control transfer: the emitted C++ falls through to the next block's label. Falling
                // off the last block has a continuation this analysis cannot see.
                if (index + 1 < blocks.Count) return new[] { index + 1 };
                throw new Unanalyzable("falls off the last block");
        }
    }

    private static void Transfer(
        IrInstruction instruction,
        State state,
        Effects effects,
        IReadOnlyDictionary<uint, Summary> summaries,
        GuestPreservationCallPolicy policy)
    {
        switch (instruction)
        {
            case IrAssign assign:
                Define(state, assign.Destination, Evaluate(state, assign.Value));
                break;
            case IrBinary binary:
                Define(state, binary.Destination, EvaluateBinary(state, binary));
                break;
            case IrLoad load:
                Define(state, load.Destination, LoadSlot(state, load.Address, load.SizeBytes));
                break;
            case IrStore store:
                StoreSlot(state, effects, store.Address, store.Source, store.SizeBytes);
                break;
            case IrResolveGuestMemoryRange resolve:
                Define(state, resolve.Destination, Value.Unknown);
                break;
            case IrResolvedLoad load:
                Define(state, load.Destination, LoadSlot(state, load.OriginalAddress, load.SizeBytes));
                break;
            case IrResolvedStore store:
                StoreSlot(state, effects, store.OriginalAddress, store.Source, store.SizeBytes);
                break;
            case IrResolvedLoadPair pair:
                {
                    var first = LoadSlot(state, pair.FirstOriginalAddress, pair.ElementSizeBytes);
                    var second = LoadSlot(state, pair.SecondOriginalAddress, pair.ElementSizeBytes);
                    Define(state, pair.FirstDestination, first);
                    Define(state, pair.SecondDestination, second);
                }
                break;
            case IrResolvedStorePair pair:
                StoreSlot(state, effects, pair.FirstOriginalAddress, pair.FirstSource, pair.ElementSizeBytes);
                StoreSlot(state, effects, pair.SecondOriginalAddress, pair.SecondSource, pair.ElementSizeBytes);
                break;
            case IrResolvedPsqLoad load:
                Define(state, load.Destination, Value.Unknown);
                break;
            case IrResolvedPsqStore store:
                KillAt(state, effects, Evaluate(state, store.OriginalAddress), 8);
                break;
            case IrCall call:
                TransferCall(call, state, effects, summaries, policy);
                break;
            case IrIndirectCall call:
                TransferEabiCall(state, effects);
                Define(state, call.Destination, Value.Unknown);
                break;
            case IrIndirectJump:
                // A tail call: the EABI target returns straight to this body's caller (perf.cfg eabi_verify
                // checks InvokeIndirectJump too).
                TransferEabiCall(state, effects);
                break;
            case IrPhi phi:
                {
                    var destination = GprIndex(phi.Destination);
                    var sameRegister = destination >= 0 &&
                                       phi.Sources.Values.All(source => GprIndex(source) == destination);
                    if (!sameRegister) Define(state, phi.Destination, Value.Unknown);
                }
                break;
            case IrSetCrField:
            case IrBranch:
            case IrJump:
            case IrJumpTable:
            case IrReturn:
            case IrComment:
            case IrTracePpc:
                break;
            default:
                // IrIndirectJump, IrUndefined and anything new.
                throw new Unanalyzable(instruction.GetType().Name);
        }
    }

    private static void TransferCall(
        IrCall call,
        State state,
        Effects effects,
        IReadOnlyDictionary<uint, Summary> summaries,
        GuestPreservationCallPolicy policy)
    {
        if (GuestTargetParser.TryParseAddress(call.Target, out var target))
        {
            if (TryThunk(target, out var kind, out var start))
            {
                TransferThunk(state, effects, kind, start);
                Define(state, call.Destination, Value.Unknown);
                return;
            }

            // Map the callee's exit values into this body's terms using the state at the call, then
            // apply the callee's stores to this body's frame.
            var callee = CalleeSummary(target, summaries, policy);
            var callerR1 = state.Gpr[1];
            var after = new Value[32];
            for (var register = 0; register < 32; ++register)
            {
                var value = callee.Exit[register];
                after[register] = value.Kind switch
                {
                    1 when value.Delta == 0 => state.Gpr[value.Register],
                    1 when state.Gpr[value.Register].IsStack =>
                        Value.Entry(1, checked(state.Gpr[value.Register].Delta + value.Delta)),
                    2 when callerR1.IsStack => ReadSlot(state, checked(callerR1.Delta + value.Delta)),
                    _ => Value.Unknown
                };
            }
            if (callee.ClobbersCallerFrame || !callerR1.IsStack)
            {
                ClobberCallerFrame(state, effects);
            }
            else
            {
                foreach (var (offset, size) in callee.CallerWrites)
                    Kill(state, effects, checked(callerR1.Delta + offset), size);
            }
            Array.Copy(after, state.Gpr, 32);
            Define(state, call.Destination, Value.Unknown);
            return;
        }

        var effect = GuestHelperEffectCatalog.Analyze(call);
        if ((effect.BoundaryFlags & OpaqueHelperFlags) != 0)
        {
            for (var register = 0; register < 32; ++register) state.Gpr[register] = Value.Unknown;
            ClobberCallerFrame(state, effects);
        }
        else
        {
            for (var register = 0; register < 32; ++register)
                if ((effect.GprWriteMask & (1u << register)) != 0) state.Gpr[register] = Value.Unknown;
        }

        if (call.Target.StartsWith("PPC_PsqSt", StringComparison.Ordinal) && call.Arguments.Count != 0)
        {
            KillAt(state, effects, Evaluate(state, call.Arguments[0]), 8);
        }
        else if (MemoryWritingHelpers.Contains(call.Target) &&
                 call.Arguments.Select(argument => Evaluate(state, argument)).Where(static v => v.IsStack)
                     .ToArray() is { Length: > 0 } stackArguments)
        {
            // A helper handed a stack address may write anywhere around it: inside this body's frame that
            // only loses its own slots, a caller-frame address also clobbers the caller's.
            state.Slots.Clear();
            if (stackArguments.Any(static v => v.Delta >= 0)) ClobberCallerFrame(state, effects);
        }
        Define(state, call.Destination, Value.Unknown);
    }

    /// <summary>
    /// Cataloged helpers (GuestHelperEffectCatalog) that store through an address argument besides
    /// PPC_PsqSt*. Every other cataloged helper only reads memory or none; uncataloged helpers are
    /// complete-context boundaries and clobber everything above.
    /// </summary>
    private static readonly HashSet<string> MemoryWritingHelpers = new(StringComparer.Ordinal)
    {
        "PPC_Stfiwx", "PPC_StoreHalfwordByteReverse", "PPC_StoreWordByteReverse", "PPC_Stswi", "PPC_Stswx",
        "PPC_Stwcx", "PPC_Ecowx"
    };

    private static void TransferEabiCall(State state, Effects effects)
    {
        for (var register = 0; register < 32; ++register)
            if ((EabiPreservedGprMask & (1u << register)) == 0) state.Gpr[register] = Value.Unknown;
        // An EABI callee may store to its caller's linkage words (back chain, LR save).
        if (state.Gpr[1].IsStack) Kill(state, effects, state.Gpr[1].Delta, 8);
        else ClobberCallerFrame(state, effects);
    }

    private static void ClobberCallerFrame(State state, Effects effects)
    {
        state.Slots.Clear();
        state.AllTouched = true;
        effects.ClobbersCallerFrame = true;
    }

    private static void Kill(State state, Effects effects, int offset, int size)
    {
        state.Kill(offset, size);
        effects.NoteStore(offset, size);
    }

    private enum ThunkKind { SaveGpr, RestGpr, SaveFpr, RestFpr }

    private static bool TryThunk(uint address, out ThunkKind kind, out int start)
    {
        var thunks = GuestSaveRestoreThunks.Current;
        if (InRange(address, thunks.SaveGpr, out start)) { kind = ThunkKind.SaveGpr; return true; }
        if (InRange(address, thunks.RestGpr, out start)) { kind = ThunkKind.RestGpr; return true; }
        if (InRange(address, thunks.SaveFpr, out start)) { kind = ThunkKind.SaveFpr; return true; }
        if (InRange(address, thunks.RestFpr, out start)) { kind = ThunkKind.RestFpr; return true; }
        kind = default;
        return false;

        static bool InRange(uint candidate, GuestSaveRestoreThunkRange? range, out int first)
        {
            first = -1;
            if (range is null) return false;
            var (baseAddress, firstRegister, lastStartRegister) = range;
            var lastAddress = baseAddress + checked((uint)((lastStartRegister - firstRegister) * 4));
            if (candidate < baseAddress || candidate > lastAddress || ((candidate - baseAddress) & 3u) != 0)
                return false;
            first = firstRegister + checked((int)((candidate - baseAddress) / 4));
            return true;
        }
    }

    /// <summary>
    /// CodeWarrior thunks address register N at r11 + (N - 32) * size (CxxLinearCodeGenerator.GuestThunkInlines).
    /// </summary>
    private static void TransferThunk(State state, Effects effects, ThunkKind kind, int start)
    {
        var frame = state.Gpr[11];
        switch (kind)
        {
            case ThunkKind.SaveGpr:
                if (!frame.IsStack)
                {
                    ClobberCallerFrame(state, effects);
                    return;
                }
                for (var register = start; register <= 31; ++register)
                {
                    var offset = checked(frame.Delta + (register - 32) * 4);
                    Kill(state, effects, offset, 4);
                    state.Slots[offset] = new Slot(4, state.Gpr[register]);
                }
                return;
            case ThunkKind.RestGpr:
                for (var register = start; register <= 31; ++register)
                    state.Gpr[register] = frame.IsStack
                        ? ReadSlot(state, checked(frame.Delta + (register - 32) * 4))
                        : Value.Unknown;
                return;
            case ThunkKind.SaveFpr:
                if (!frame.IsStack)
                {
                    ClobberCallerFrame(state, effects);
                    return;
                }
                for (var register = start; register <= 31; ++register)
                    Kill(state, effects, checked(frame.Delta + (register - 32) * 8), 8);
                return;
            case ThunkKind.RestFpr:
                return;
        }
    }

    /// <summary>
    /// The word at entry r1 + <paramref name="offset"/>: a tracked slot, or, in the caller's frame (at or
    /// above entry r1) where this body has not stored, the caller's own word as it was at entry.
    /// </summary>
    private static Value ReadSlot(State state, int offset)
    {
        if (state.Slots.TryGetValue(offset, out var slot)) return slot.Size == 4 ? slot.Value : Value.Unknown;
        foreach (var (key, other) in state.Slots)
            if (key < offset + 4 && offset < key + other.Size) return Value.Unknown;
        return offset >= 0 && !state.CallerWordTouched(offset) ? Value.CallerSlot(offset) : Value.Unknown;
    }

    private static Value LoadSlot(State state, IrAddress address, int size)
    {
        var baseValue = Evaluate(state, IrValue.Register(address.Base));
        if (size != 4 || !baseValue.IsStack) return Value.Unknown;
        return ReadSlot(state, checked(baseValue.Delta + address.Offset));
    }

    private static void StoreSlot(State state, Effects effects, IrAddress address, IrValue source, int size)
    {
        var baseValue = Evaluate(state, IrValue.Register(address.Base));
        if (!baseValue.IsStack) return;
        var offset = checked(baseValue.Delta + address.Offset);
        Kill(state, effects, offset, size);
        if (size == 4) state.Slots[offset] = new Slot(4, Evaluate(state, source));
    }

    private static void KillAt(State state, Effects effects, Value address, int size)
    {
        if (address.IsStack) Kill(state, effects, address.Delta, size);
    }

    private static Value EvaluateBinary(State state, IrBinary binary)
    {
        if (!binary.Op.Equals("add", StringComparison.Ordinal)) return Value.Unknown;
        var left = Evaluate(state, binary.Left);
        var right = Evaluate(state, binary.Right);
        if (left.IsStack && binary.Right.Kind == "const" && binary.Right.Constant is { } rightConstant &&
            rightConstant is >= int.MinValue and <= int.MaxValue)
            return Value.Entry(1, checked(left.Delta + (int)rightConstant));
        if (right.IsStack && binary.Left.Kind == "const" && binary.Left.Constant is { } leftConstant &&
            leftConstant is >= int.MinValue and <= int.MaxValue)
            return Value.Entry(1, checked(right.Delta + (int)leftConstant));
        return Value.Unknown;
    }

    private static Value Evaluate(State state, IrValue value)
    {
        if (value.Kind != "register" || string.IsNullOrEmpty(value.RegisterName)) return Value.Unknown;
        var gpr = GprIndex(value.RegisterName);
        if (gpr >= 0) return state.Gpr[gpr];
        return state.Temps.TryGetValue(value.RegisterName, out var temp) ? temp : Value.Unknown;
    }

    private static void Define(State state, string? name, Value value)
    {
        if (string.IsNullOrWhiteSpace(name)) return;
        var gpr = GprIndex(name);
        if (gpr >= 0)
        {
            state.Gpr[gpr] = value;
            return;
        }
        if (IsArchitecturalNonGpr(name)) return;
        if (value == Value.Unknown) state.Temps.Remove(name);
        else state.Temps[name] = value;
    }

    /// <summary>GPR number of an architectural name or its SSA version (<c>r31</c>, <c>r31_4</c>), else -1.</summary>
    internal static int GprIndex(string name)
    {
        var span = name.AsSpan();
        var underscore = span.IndexOf('_');
        if (underscore > 0)
        {
            for (var index = underscore + 1; index < span.Length; ++index)
                if (!char.IsDigit(span[index])) return -1;
            if (underscore + 1 == span.Length) return -1;
            span = span[..underscore];
        }
        if (span.Length < 2 || span.Length > 3 || span[0] != 'r') return -1;
        return int.TryParse(span[1..], out var number) && number is >= 0 and < 32 ? number : -1;
    }

    private static bool IsArchitecturalNonGpr(string name)
    {
        var underscore = name.IndexOf('_');
        var baseName = underscore > 0 ? name[..underscore] : name;
        if (baseName.Length >= 2 && baseName[0] == 'f' && int.TryParse(baseName.AsSpan(1), out var fpr) &&
            fpr is >= 0 and < 32)
            return true;
        return baseName is "lr" or "ctr" or "xer" or "cr" or "fpscr" or "msr" or "srr0" or "srr1" ||
               baseName.StartsWith("cr", StringComparison.Ordinal) ||
               baseName.StartsWith("gqr", StringComparison.Ordinal) ||
               baseName.StartsWith("hid", StringComparison.Ordinal);
    }

    /// <summary>
    /// Why functions miss a complete proof (diagnostics, KARTPAD_PRESERVATION_REPORT): per function the
    /// first blocking cause, then the callees most often responsible and the smallest intrinsic failures.
    /// </summary>
    public static IReadOnlyList<string> Report(
        IReadOnlyDictionary<uint, IrFunction> functions,
        IReadOnlyDictionary<uint, uint> preserved,
        GuestPreservationCallPolicy policy)
    {
        var causes = new Dictionary<string, int>(StringComparer.Ordinal);
        var culprits = new Dictionary<uint, int>();
        var culpritKind = new Dictionary<uint, string>();
        var intrinsic = new List<(uint Address, int Count)>();
        foreach (var (address, function) in functions)
        {
            if (preserved.GetValueOrDefault(address) == TrackedGprMask) continue;
            string cause;
            var instructions = function.Blocks.SelectMany(static block => block.Instructions).ToList();
            if (instructions.Any(static i => i is IrUndefined)) cause = "body: undefined instruction";
            else
            {
                cause = "dataflow (stack/register pattern)";
                foreach (var call in instructions.OfType<IrCall>())
                {
                    if (GuestTargetParser.TryParseAddress(call.Target, out var target))
                    {
                        if (TryThunk(target, out _, out _)) continue;
                        if (CalleePreservedGprs(target, preserved, policy) == TrackedGprMask) continue;
                        var kind = policy.NativeTargets.Contains(target) ? "native (non-EABI)"
                            : policy.ModOverridableTargets.Contains(target) ? "mod-overridable"
                            : !policy.Contracts.ContainsKey(target) ? "no contract"
                            : functions.ContainsKey(target) ? "translated, unproven"
                            : "other";
                        cause = "callee: " + kind;
                        culprits[target] = culprits.GetValueOrDefault(target) + 1;
                        culpritKind[target] = kind;
                        break;
                    }
                    var effect = GuestHelperEffectCatalog.Analyze(call);
                    if ((effect.BoundaryFlags & OpaqueHelperFlags) != 0)
                    {
                        cause = "helper: " + call.Target;
                        break;
                    }
                }
                if (cause.StartsWith("dataflow", StringComparison.Ordinal))
                    intrinsic.Add((address, instructions.Count));
            }
            causes[cause] = causes.GetValueOrDefault(cause) + 1;
        }
        var lines = new List<string>();
        foreach (var pair in causes.OrderByDescending(static pair => pair.Value))
            lines.Add($"cause {pair.Value,7} {pair.Key}");
        foreach (var pair in culprits.Where(pair => culpritKind[pair.Key] != "translated, unproven")
                     .OrderByDescending(static pair => pair.Value).Take(25))
            lines.Add($"callee {pair.Value,6} 0x{pair.Key:X8} {culpritKind[pair.Key]}");
        foreach (var pair in culprits.Where(pair => culpritKind[pair.Key] == "translated, unproven")
                     .OrderByDescending(static pair => pair.Value).Take(30))
            lines.Add($"unproven-callee {pair.Value,6} 0x{pair.Key:X8} mask=0x{preserved.GetValueOrDefault(pair.Key):X8} " +
                      $"{functions[pair.Key].Blocks.Sum(static b => b.Instructions.Count)} IR");
        // Follow each unproven function's first blocking translated callee down to a root (a body that
        // fails on its own, or a non-translated blocker), and rank roots by how many functions they block.
        var firstBlocker = new Dictionary<uint, uint>();
        foreach (var (address, function) in functions)
        {
            if (preserved.GetValueOrDefault(address) == TrackedGprMask) continue;
            foreach (var call in function.Blocks.SelectMany(static b => b.Instructions).OfType<IrCall>())
            {
                if (!GuestTargetParser.TryParseAddress(call.Target, out var target) || TryThunk(target, out _, out _))
                    continue;
                if (CalleePreservedGprs(target, preserved, policy) == TrackedGprMask) continue;
                firstBlocker[address] = target;
                break;
            }
        }
        var rootCounts = new Dictionary<uint, int>();
        foreach (var address in firstBlocker.Keys)
        {
            var root = address;
            var seen = new HashSet<uint>();
            while (firstBlocker.TryGetValue(root, out var next) && functions.ContainsKey(next) &&
                   !policy.ModOverridableTargets.Contains(next) && seen.Add(root))
                root = next;
            if (firstBlocker.TryGetValue(root, out var external) &&
                (!functions.ContainsKey(external) || policy.ModOverridableTargets.Contains(external)))
                root = external;
            rootCounts[root] = rootCounts.GetValueOrDefault(root) + 1;
        }
        foreach (var pair in rootCounts.OrderByDescending(static pair => pair.Value).Take(30))
        {
            var kind = policy.ModOverridableTargets.Contains(pair.Key) ? "mod-overridable"
                : functions.TryGetValue(pair.Key, out var body)
                    ? $"mask=0x{preserved.GetValueOrDefault(pair.Key):X8} {body.Blocks.Sum(static b => b.Instructions.Count)} IR"
                    : "non-translated";
            lines.Add($"root {pair.Value,6} 0x{pair.Key:X8} {kind}");
        }
        var intrinsicMasks = intrinsic.GroupBy(item => preserved.GetValueOrDefault(item.Address))
            .OrderByDescending(static g => g.Count()).Take(12);
        foreach (var group in intrinsicMasks)
            lines.Add($"intrinsic-mask 0x{group.Key:X8} {group.Count()} function(s)");
        foreach (var (address, count) in intrinsic.OrderBy(static item => item.Count).Take(40))
            lines.Add($"dataflow 0x{address:X8} {count} IR mask=0x{preserved.GetValueOrDefault(address):X8}");
        var histogram = new int[33];
        foreach (var mask in preserved.Values) histogram[System.Numerics.BitOperations.PopCount(mask)]++;
        lines.Add("popcount histogram (tracked=21): " + string.Join(" ",
            histogram.Select((count, bits) => (count, bits)).Where(static p => p.count != 0)
                .Select(static p => $"{p.bits}:{p.count}")));
        return lines;
    }
}

/// <summary>
/// Process-wide post-call reload exemptions for code generation: per direct-call target, the GPRs the
/// callee returns unchanged (GuestNonvolatilePreservationAnalyzer.CalleePreservedGprs, precomputed by
/// translate-recursive). Set once before emission, same pattern as <see cref="GuestSaveRestoreThunks.Current"/>.
/// translate-recursive writes the table next to its output metadata (<see cref="FileName"/>) so translate-mod
/// (Retro Rewind overlay bodies, which have no base contracts) gets the same exemptions.
/// Environment variable <c>KARTPAD_RELOAD_ELISION=0</c> turns the elision off and reproduces the pre-b73 output.
/// </summary>
public static class GuestNonvolatilePreservation
{
    public const string FileName = "nonvolatile_reload_exemptions.json";

    private static IReadOnlyDictionary<uint, uint> _exemptions = new Dictionary<uint, uint>();

    public static bool Enabled { get; } =
        Environment.GetEnvironmentVariable("KARTPAD_RELOAD_ELISION") != "0";

    public static void Install(IReadOnlyDictionary<uint, uint> exemptions) => _exemptions = exemptions;

    public static IReadOnlyDictionary<uint, uint> BuildExemptions(
        IEnumerable<uint> targets,
        IReadOnlyDictionary<uint, uint> proven,
        GuestPreservationCallPolicy policy)
    {
        var table = new SortedDictionary<uint, uint>();
        foreach (var target in targets)
        {
            var mask = GuestNonvolatilePreservationAnalyzer.CalleePreservedGprs(target, proven, policy);
            if (mask != 0) table[target] = mask;
        }
        return table;
    }

    public static void Write(string path, IReadOnlyDictionary<uint, uint> exemptions)
    {
        var builder = new System.Text.StringBuilder();
        builder.Append("{\"format\":\"kartpad-nonvolatile-reload-exemptions\",\"formatVersion\":1,\"entries\":{");
        var first = true;
        foreach (var (address, mask) in exemptions.OrderBy(static pair => pair.Key))
        {
            if (!first) builder.Append(',');
            first = false;
            builder.Append("\"0x").Append(address.ToString("X8")).Append("\":").Append(mask);
        }
        builder.Append("}}\n");
        System.IO.File.WriteAllText(path, builder.ToString());
    }

    public static IReadOnlyDictionary<uint, uint>? TryRead(string path)
    {
        if (!System.IO.File.Exists(path)) return null;
        using var document = System.Text.Json.JsonDocument.Parse(System.IO.File.ReadAllText(path));
        var table = new Dictionary<uint, uint>();
        foreach (var entry in document.RootElement.GetProperty("entries").EnumerateObject())
            table[Convert.ToUInt32(entry.Name[2..], 16)] = entry.Value.GetUInt32();
        return table;
    }

    /// <summary>GPRs whose reload after a direct call to <paramref name="target"/> is redundant.</summary>
    public static uint DirectCallExemption(uint target) =>
        Enabled ? _exemptions.GetValueOrDefault(target) : 0;

    /// <summary>GPRs an indirect call is assumed (EABI) to preserve.</summary>
    public static uint IndirectCallExemption =>
        Enabled ? GuestNonvolatilePreservationAnalyzer.EabiPreservedGprMask : 0;
}
