#pragma once
#include "ppc_runtime.h"
#include "isa/ppc_isa_cr.h"
#include "memory_access.h"
#include <array>
#include <cstring>

// Whole-loop native implementations. Host views are valid only until the next
// guest call. Scalar page biases exclude deferred reads and executable writes;
// unusual mappings use the original checked memory operations at each access.
namespace NativeSubsystem {
inline uint8_t* Range(uint32_t a, uint32_t n, bool write = false) {
    if (!n || (a & 0xe0000000u) != 0x80000000u || a > UINT32_MAX - (n - 1)) return nullptr;
    const auto page = a >> MemoryInline::kPageShift;
    if (((a + n - 1) >> MemoryInline::kPageShift) != page) return nullptr;
    const auto bias = write ? MemoryInline::g_scalarWritablePageBias[page]
                            : MemoryInline::g_scalarReadablePageBias[page];
    return bias ? reinterpret_cast<uint8_t*>(bias - 1u + a) : nullptr;
}
inline bool Overlap(uint32_t a, uint32_t n, uint32_t b, uint32_t m) {
    return uint64_t(a) < uint64_t(b) + m && uint64_t(b) < uint64_t(a) + n;
}
inline uint32_t Word(const uint8_t* p) {
    uint32_t v; std::memcpy(&v, p, 4); return __builtin_bswap32(v);
}
inline uint16_t Half(const uint8_t* p) {
    uint16_t v; std::memcpy(&v, p, 2); return __builtin_bswap16(v);
}
inline void Word(uint8_t* p, uint32_t v) {
    v = __builtin_bswap32(v); std::memcpy(p, &v, 4);
}
inline void Half(uint8_t* p, uint16_t v) {
    v = __builtin_bswap16(v); std::memcpy(p, &v, 2);
}
inline double Float(const uint8_t* p) { return PpcBitCastToFloatInline(Word(p)); }
inline void Double(uint8_t* p, double v) {
    uint64_t bits = __builtin_bswap64(PpcBitCastToU64Inline(v)); std::memcpy(p, &bits, 8);
}
struct View {
    uint32_t address, size;
    uint8_t* read;
    View(uint32_t a, uint32_t n, bool = false)
        : address(a), size(n), read(n && a<=UINT32_MAX-(n-1) ? MKW_FLAT_GUEST_BASE+a : nullptr) {}
    uint32_t U32(uint32_t o) const { return read && o <= size - 4 ? Word(read+o) : MemoryInline::FlatRead32(address+o); }
    uint16_t U16(uint32_t o) const { return read && o <= size - 2 ? Half(read+o) : MemoryInline::FlatRead16(address+o); }
    double F32(uint32_t o) const { return read && o <= size - 4 ? Float(read+o) : MemoryInline::FlatReadFloat32(address+o); }
    void W32(uint32_t o, uint32_t v) const {
        if (read && o <= size - 4 && !MemoryInline::FlatWriteNeedsPolicy(address+o)) Word(read+o,v);
        else MemoryInline::FlatWrite32(address+o,v);
    }
    void W16(uint32_t o, uint16_t v) const {
        if (read && o <= size - 2 && !MemoryInline::FlatWriteNeedsPolicy(address+o)) Half(read+o,v);
        else MemoryInline::FlatWrite16(address+o,v);
    }
};

// Effect particle lifecycle passes: keep the node view in registers, with no
// repeated page lookup for its state/counter fields. Link offset is reread after
// writes, preserving even unusual aliases with the manager or another node.
inline bool ParticlePass(CpuContext* MKW_RESTRICT c, bool begin) {
    if (!Range(c->gpr[3],88,true) || !Range(c->gpr[3],88)) return false;
    const View manager(c->gpr[3],88,true);
    uint32_t r0=c->gpr[0], r5=c->gpr[5];
    const bool updateAll=c->gpr[4]==0;
    if (begin) manager.W32(84,0);
    uint32_t node=manager.U32(56);
    while (node) {
        const View p(node,228,true);
        if (begin) {
            if (!updateAll) r0=p.U16(226);
            if (updateAll || r0) {
                r5=p.U16(226);
                if (r5) { r0=r5-1; p.W16(226,uint16_t(r0)); }
                r0=p.U32(12);
                if (r0==1) { r0=p.U32(216); if (r0==1) p.W32(216,0); }
            }
        } else {
            r0=p.U32(12);
            if (r0==1) { r0=p.U32(216); if (r0==3) p.W32(216,1); }
        }
        r0=manager.U16(66);
        if (begin) r5=node+r0;
        node=p.U32(r0+4);
    }
    SetCRResident(c->cr,c->xer,0,int32_t(node),int32_t(0));
    c->gpr[0]=r0;
    if (begin) { c->gpr[5]=r5; c->gpr[6]=0; c->gpr[7]=node; }
    else { c->gpr[4]=node; c->gpr[5]=1; }
    return true;
}

// Shared intrusive-list snapshot used by effects. Output may overlap a node:
// retain store-before-next semantics. Unsupported output/header layouts use the
// translated fallback, keeping the default-on path restricted to the fast loop.
inline bool GatherList(CpuContext* MKW_RESTRICT c) {
    if (!Range(c->gpr[3],12) || !Range(c->gpr[1]-32u,40,true)) return false;
    const uint32_t sp=c->gpr[1], list=c->gpr[3], out=c->gpr[4], limit=c->gpr[5];
    auto* stack=Range(sp-32,40,true);
    if (Overlap(sp-32,40,list,12)) return false;
    const uint32_t outputBytes=limit<=65535 ? 4u*(limit ? limit : 1u) : 0;
    auto* output=Range(out,outputBytes,true);
    if (!output || Overlap(out,outputBytes,list,12)) return false;
    Word(stack,sp); Word(stack+36,c->lr);
    for (unsigned i=28;i<32;++i) Word(stack+16+4*(i-28),c->gpr[i]);
    uint32_t count=0, node=0, r5=limit;
    {
        // With a disjoint list header, its link offset cannot change during
        // this call. One output proof covers all stores. CR0 is unobserved
        // inside this callback-free loop, so publish only its final comparison.
        const uint32_t link=MemoryInline::FlatRead16(list+10);
        node=MemoryInline::FlatRead32(list);
        while (node) {
            r5=count*4; ++count;
            Word(output+r5,node);
            if (count>=limit) break;
            node=MemoryInline::FlatRead32(node+link+4);
        }
        if (node) SetCRResident(c->cr,c->xer,0,int32_t(count),int32_t(limit));
        else SetCRResident(c->cr,c->xer,0,int32_t(0),int32_t(0));
    }
    // Reload saved state because an overlapping output can change the frame.
    for (unsigned i=28;i<32;++i) c->gpr[i]=MemoryInline::FlatRead32(sp-16+4*(i-28));
    c->gpr[0]=MemoryInline::FlatRead32(sp+4); c->lr=c->gpr[0];
    c->gpr[3]=count; c->gpr[4]=node; c->gpr[5]=r5;
    return true;
}

// Course octree search. Resolve controller fields once and traverse through
// the runtime's flat RAM loads. No geometry cache can become
// stale when RR loads a new course or modifies its collision data.
inline bool CollisionLookup(CpuContext* MKW_RESTRICT c) {
    const uint32_t sp=c->gpr[1], controller=c->gpr[3], position=c->gpr[4];
    if (!Range(controller,56) || !Range(position,12) || !Range(sp-16u,16,true)) return false;
    if (Overlap(sp-16,16,controller,56) || Overlap(sp-16,16,position,12)) return false;
    const View k(controller,56);
    auto* stack=Range(sp-16,16,true);
    Word(stack,sp);
    uint32_t r0=c->gpr[0],r3=controller,r4=position,r5=c->gpr[5],r6=c->gpr[6],r7=c->gpr[7];
    uint32_t xyz[3]={c->gpr[8],c->gpr[9],c->gpr[10]},shift=c->gpr[11];
    PPC_FPR f0=c->fpr[0],f1=c->fpr[1];
    for (unsigned axis=0;axis<3;++axis) {
        f1.d=MemoryInline::FlatReadFloat32(position+4*axis);
        f0.d=k.F32(20+4*axis);
        r0=k.U32(32+4*axis);
        PpcFsubsStateInline(f0.d,f1.d,f0.d);
        PpcFctiwzStateInline(f0.d,f0.d);
        Double(stack+8,f0.d);
        xyz[axis]=Word(stack+12);
        r0 &= xyz[axis];
        SetCRResident(c->cr,c->xer,0,int32_t(r0),int32_t(0));
        if (r0) { r3=0; goto finish; }
    }
    shift=k.U32(44); r5=k.U32(52);
    r6=PPC_Srw(xyz[2],shift); r4=PPC_Srw(xyz[1],shift); r7=PPC_Srw(xyz[0],shift);
    r5=PPC_Slw(r6,r5); r0=PPC_Slw(r4,k.U32(48)); r6=k.U32(12);
    r0=PpcRotl32Inline(r5|r0|r7,2)&~3u;
    {
        for (;;) {
            r3=MemoryInline::FlatRead32(r6+r0);
            r0=r3 & 0x80000000u;
            SetCRResident(c->cr,c->xer,0,int32_t(r0),int32_t(0));
            if (r0) { r0=r3 & 0x7fffffffu; r3=r6+r0; break; }
            --shift; r6+=r3;
            r0=PPC_Srw(xyz[2],shift); r4=PPC_Srw(xyz[0],shift); r3=PPC_Srw(xyz[1],shift);
            r0=((r0&1u)<<2)|((r3&1u)<<1)|(r4&1u); r0*=4;
        }
    }
finish:
    c->fpr[0]=f0; c->fpr[1]=f1;
    c->gpr[0]=r0; c->gpr[3]=r3; c->gpr[4]=r4; c->gpr[5]=r5; c->gpr[6]=r6; c->gpr[7]=r7;
    c->gpr[8]=xyz[0]; c->gpr[9]=xyz[1]; c->gpr[10]=xyz[2]; c->gpr[11]=shift;
    return true;
}

inline bool MatrixCopy(CpuContext* MKW_RESTRICT c) {
    if (c->gqr[0]) return false;
    const auto* input=Range(c->gpr[3],48);
    auto* output=Range(c->gpr[4],48,true);
    if (!input || !output) return false;
    // Pair-at-a-time preserves partial-overlap behavior of the SDK routine.
    for (unsigned i=0;i<6;++i) {
        c->fpr[i].d=PpcLoadPairPsqFloatFromHostInline(input+8*i);
        PpcStorePairPsqFloatToHostInline(output+8*i,c->fpr[i].d);
    }
    return true;
}

// Common scene world-matrix cases: unchanged parent matrix, local-only matrix,
// or a disabled update request. Concatenation/billboard callbacks retain the
// full original routine; no native pointer is retained across guest calls.
inline bool SceneWorldCopy(CpuContext* MKW_RESTRICT c) {
    const uint32_t object=c->gpr[3], parent=c->gpr[4], request=c->gpr[5], sp=c->gpr[1];
    const auto* obj=Range(object,208);
    auto* stack=Range(sp-16u,24,true);
    if (!obj || !stack || c->gqr[0] || Overlap(sp-16,24,object,208)) return false;
    uint8_t* req=nullptr;
    uint32_t requestFlags=c->gpr[6];
    if (request) {
        req=Range(request,4,true);
        if (!req || !Range(request,4) || Overlap(request,4,sp-16,24) || Overlap(request,4,object,208)) return false;
        requestFlags=Word(Range(request,4));
    }
    const bool skip=request && (requestFlags&1);
    const uint32_t flags=Word(obj+204);
    const uint32_t source=parent ? parent : object+12;
    const auto* input=Range(source,48);
    auto* output=Range(object+60,48,true);
    if (!skip && ((flags&0x10000000u) || (parent && !(flags&0x80000000u)) ||
        !input || !output || Overlap(sp-16,24,source,48))) return false;
    Word(stack,sp); Word(stack+20,c->lr); Word(stack+12,c->gpr[31]);
    if (request) {
        c->gpr[6]=requestFlags;
        SetCRResident(c->cr,c->xer,0,int32_t(requestFlags&1),int32_t(0));
    }
    if (skip) Word(req,requestFlags&~1u);
    else {
        for (unsigned i=0;i<6;++i) {
            c->fpr[i].d=PpcLoadPairPsqFloatFromHostInline(input+8*i);
            PpcStorePairPsqFloatToHostInline(output+8*i,c->fpr[i].d);
        }
        if (parent) for (unsigned i=0;i<6;++i) c->fpr[i].d=PPC_PsToScalarInline(c->fpr[i].d);
        c->gpr[3]=source; c->gpr[4]=object+60;
        SetCRResident(c->cr,c->xer,0,int32_t(flags&0x10000000u),int32_t(0));
    }
    c->gpr[0]=c->lr;
    return true;
}

inline bool ParticleDirection(CpuContext* MKW_RESTRICT c) {
    const uint32_t sp=c->gpr[1], particle=c->gpr[3], out=c->gpr[4];
    if (c->gqr[0] || !Range(particle,196) || !Range(out,12,true) || !Range(sp-32u,32,true)) return false;
    if (Overlap(sp-32,32,particle,196) || Overlap(sp-32,32,out,12)) return false;
    auto* stack=Range(sp-32,32,true);
    const auto* p=Range(particle,196);
    auto* dest=Range(out,12,true);
    Word(stack,sp);
    c->fpr[0].d=PpcLoadPairPsqFloatFromHostInline(p+172);
    c->fpr[1].d=PpcLoadPairPsqFloatFromHostInline(p+184);
    c->fpr[3].d=PPC_PsSubInline(c->fpr[0].d,c->fpr[1].d);
    c->fpr[0].d=PpcMakePairedResultInline(PpcBitCastToFloatInline(Word(p+180)),1.0f).d;
    c->fpr[1].d=PpcMakePairedResultInline(PpcBitCastToFloatInline(Word(p+192)),1.0f).d;
    PpcStorePairPsqFloatToHostInline(stack+8,c->fpr[3].d);
    c->fpr[3].d=PPC_PsSubInline(c->fpr[0].d,c->fpr[1].d);
    c->fpr[2].d=Float(stack+8);
    Word(stack+16,PpcConvertToSingleFTZInline(PpcBitCastToU64Inline(double(PpcGetPs0Inline(c->fpr[3].d)))));
    c->fpr[1].d=Float(stack+12); c->fpr[0].d=Float(stack+16);
    Word(dest,PpcBitCastToU32Inline(float(c->fpr[2].d)));
    Word(dest+4,PpcBitCastToU32Inline(float(c->fpr[1].d)));
    Word(dest+8,PpcBitCastToU32Inline(float(c->fpr[0].d)));
    c->gpr[3]=out; c->gpr[5]=sp-24;
    return true;
}

// Hermite animation channel: resolve the packed key array once, then walk and
// interpolate through native pointers. Keep the PPC estimate refinement and
// rounding sequence so animation results and volatile FPRs remain identical.
inline bool Keyframe(CpuContext* MKW_RESTRICT c) {
    const uint32_t base=c->gpr[3], sp=c->gpr[1];
    const auto* header=Range(base,8);
    if (!header || c->gqr[3]!=0x00050005u || !std::isfinite(c->fpr[1].d)) return false;
    const uint32_t count=Half(header), bytes=8+12*count;
    if (!count || count>4096) return false;
    const auto* keys=Range(base,bytes);
    auto* stack=Range(sp-16u,16,true);
    const uint8_t* constants=nullptr;
    if (!keys || !stack || Overlap(sp-16,16,base,bytes)) return false;
    const double frame=c->fpr[1].d, first=Float(keys+8), last=Float(keys+8+12*(count-1));
    if (!std::isfinite(first) || !std::isfinite(last)) return false;
    // Read-only bounded search before any stores or FPSCR changes. Malformed
    // key layouts/estimates leave the complete call on the original path.
    int selected=0;
    if (frame>first && frame<last) {
        constants=Range(c->gpr[2]-29840u,12);
        if (!constants || Overlap(sp-16,16,c->gpr[2]-29840u,12)) return false;
        const double scale=Float(keys+4);
        const float delta=float(frame-first), weighted=float(double(delta)*count);
        const float estimate=float(scale*double(weighted));
        if (!std::isfinite(estimate)) return false;
        selected=int(static_cast<uint16_t>(PpcScaleAndClampPsqInline<uint16_t>(estimate,0)));
        if (selected>=int(count)) return false;
        double sample=Float(keys+8+12*selected);
        if (!std::isfinite(sample)) return false;
        if (frame<sample) {
            while (frame<sample) {
                if (--selected<0) return false;
                sample=Float(keys+8+12*selected); if (!std::isfinite(sample)) return false;
            }
        } else {
            do {
                if (++selected>=int(count)) return false;
                sample=Float(keys+8+12*selected); if (!std::isfinite(sample)) return false;
            } while (sample<=frame);
            --selected;
        }
        if (selected+1>=int(count)) return false;
    }
    c->gpr[5]=count; c->fpr[2].d=first; c->gpr[0]=(count-1)*12;
    Word(stack,sp); c->gpr[4]=base+c->gpr[0];
    PpcCompareStateInline(true,frame,first); SetCRFloatResident(c->cr,0,frame,first);
    c->cr=PpcCrLogicalResident(c->cr,7,2,0,2);
    if (frame<=first) { c->fpr[1].d=Float(keys+12); return true; }
    c->fpr[0].d=last;
    PpcCompareStateInline(true,last,frame); SetCRFloatResident(c->cr,0,last,frame);
    c->cr=PpcCrLogicalResident(c->cr,7,2,0,2);
    if (last<=frame) { c->fpr[1].d=Float(keys+12+12*(count-1)); return true; }
    std::array<PPC_FPR,12> f;
    std::memcpy(f.data(),c->fpr,sizeof(f));
    const auto finish=[&](){std::memcpy(c->fpr,f.data(),sizeof(f));return true;};
    Half(stack+8,uint16_t(count));
    PpcFsubsStateInline(f[3].d,frame,f[2].d);
    f[2].d=PpcMakePairedResultInline(float(count),1.0f).d;
    f[0].d=Float(keys+4);
    PpcFmulsStateInline(f[2].d,f[3].d,PPC_PsToScalarInline(f[2].d));
    PpcFmulsStateInline(f[0].d,f[0].d,f[2].d);
    Half(stack+12,static_cast<uint16_t>(PpcScaleAndClampPsqInline<uint16_t>(float(f[0].d),0)));
    c->gpr[4]=sp-4; c->gpr[0]=Half(stack+12)*12;
    // The bounded search already identified this segment. All its comparisons
    // were finite and only wrote CR0/FPCC, both overwritten by the comparison
    // below. Preserve its last volatile load without repeating the search.
    const double initial=Float(keys+8+c->gpr[0]);
    f[0].d=Float(keys+8+12*(selected+(frame<initial ? 0 : 1)));
    c->gpr[3]=base+8+12*selected;
    const auto* key=keys+8+12*selected;
    f[3].d=Float(key);
    PpcCompareStateInline(false,f[3].d,frame); SetCRFloatResident(c->cr,0,f[3].d,frame);
    if (f[3].d==frame) { f[1].d=Float(key+4); return finish(); }
    f[2].d=Float(key+12);
    PpcFsubsStateInline(f[10].d,frame,f[3].d);
    f[0].d=Float(constants+4);
    PpcFsubsStateInline(f[11].d,f[2].d,f[3].d);
    f[2].d=Float(key+16); f[5].d=Float(key+4); f[1].d=Float(constants+8);
    f[8].d=PpcFresValueStateInline(f[8].d,f[11].d);
    f[4].d=Float(constants); f[7].d=Float(key+20); f[6].d=Float(key+8);
    f[9].d=PPC_PsAddInline(f[8].d,f[8].d);
    f[8].d=PPC_PsMulInline(f[8].d,f[8].d);
    PpcFsubsStateInline(f[3].d,f[5].d,f[2].d);
    f[8].d=PPC_PsNmsubInline(PPC_PsFromScalarInline(f[11].d),f[8].d,f[9].d);
    PpcFmulsStateInline(f[9].d,f[10].d,PPC_PsToScalarInline(f[8].d));
    PpcFmulsStateInline(f[2].d,f[0].d,f[9].d);
    PpcFsubsStateInline(f[8].d,f[9].d,f[4].d);
    PpcFmulsStateInline(f[0].d,f[9].d,f[7].d);
    PpcFsubsStateInline(f[4].d,f[2].d,f[1].d);
    PpcFmulsStateInline(f[1].d,f[8].d,f[6].d);
    PpcFmulsStateInline(f[2].d,f[10].d,f[8].d);
    PpcFmulsStateInline(f[3].d,f[4].d,f[3].d);
    PpcFaddsStateInline(f[0].d,f[1].d,f[0].d);
    PpcFmulsStateInline(f[1].d,f[9].d,f[3].d);
    PpcFmulsStateInline(f[0].d,f[2].d,f[0].d);
    PpcFmulsStateInline(f[1].d,f[9].d,f[1].d);
    PpcFaddsStateInline(f[1].d,f[5].d,f[1].d);
    PpcFaddsStateInline(f[1].d,f[1].d,f[0].d);
    return finish();
}

// Native scene vertex-processing traversal (event 3). Virtual children retain
// the runtime dispatcher, so mod implementations remain active. Read count and
// child table after each callback; callbacks may change the graph.
template<class Dispatch>
bool SceneVertexWalk(CpuContext* c, Dispatch dispatch) {
    if (c->gpr[4]!=3 || !Range(c->gpr[3],232) || !Range(c->gpr[1]-48u,56,true)) return false;
    const uint32_t sp=c->gpr[1], group=c->gpr[3];
    if (Overlap(sp-48,56,group,232)) return false;
    auto* stack=Range(sp-48,56,true);
    Word(stack,sp); Word(stack+52,c->lr);
    for (unsigned i=26;i<32;++i) Word(stack+24+4*(i-26),c->gpr[i]);
    c->gpr[1]=sp-48; c->gpr[0]=c->lr; c->gpr[11]=sp;
    c->gpr[29]=group; c->gpr[26]=3; c->gpr[30]=c->gpr[5]; c->gpr[31]=c->gpr[6];
    SetCRResident(c->cr,c->xer,0,int32_t(3),int32_t(2));
    c->gpr[27]=c->gpr[28]=0;
    for (;;) {
        c->gpr[0]=MemoryInline::FlatRead32(c->gpr[29]+228);
        SetCRResident(c->cr,c->xer,0,c->gpr[27],c->gpr[0]);
        if (!(c->cr & 0x80000000u)) break;
        c->gpr[3]=MemoryInline::FlatRead32(c->gpr[29]+220);
        c->gpr[4]=c->gpr[26]; c->gpr[5]=c->gpr[30]; c->gpr[6]=c->gpr[31];
        c->gpr[3]=MemoryInline::FlatRead32(c->gpr[3]+c->gpr[28]);
        c->gpr[12]=MemoryInline::FlatRead32(c->gpr[3]);
        c->gpr[12]=MemoryInline::FlatRead32(c->gpr[12]+12);
        c->ctr=c->gpr[12]; c->lr=0x8006eb28u;
        // No resolved host view crosses this call or a fiber suspension.
        dispatch(c->ctr,c);
        c->gpr[28]+=4; ++c->gpr[27];
    }
    c->gpr[11]=c->gpr[1]+48;
    for (unsigned i=26;i<32;++i) c->gpr[i]=MemoryInline::FlatRead32(c->gpr[11]-24+4*(i-26));
    c->gpr[0]=MemoryInline::FlatRead32(c->gpr[1]+52); c->lr=c->gpr[0]; c->gpr[1]+=48;
    return true;
}
} // namespace NativeSubsystem
