using System.Buffers.Binary;

if (args.Length != 1) return 2;
const uint entry = 0x80001000, output = 0x80010000, data = 0x80011000;
static uint Lis(int rd,uint v)=>0x3c000000u|((uint)rd<<21)|((v>>16)&0xffffu);
static uint Ori(int rd,int ra,uint v)=>0x60000000u|((uint)ra<<21)|((uint)rd<<16)|(v&0xffffu);
static uint Addi(int rd,int ra,int v)=>0x38000000u|((uint)rd<<21)|((uint)ra<<16)|(uint)(v&0xffff);
static uint Stw(int rs,int ra,int d)=>0x90000000u|((uint)rs<<21)|((uint)ra<<16)|(uint)(d&0xffff);
static uint Add(int rd,int ra,int rb)=>0x7c000214u|((uint)rd<<21)|((uint)ra<<16)|((uint)rb<<11);
static uint Lfs(int fd,int ra,int d)=>0xc0000000u|((uint)fd<<21)|((uint)ra<<16)|(uint)(d&0xffff);
static uint Stfs(int fs,int ra,int d)=>0xd0000000u|((uint)fs<<21)|((uint)ra<<16)|(uint)(d&0xffff);
static uint Fadds(int fd,int fa,int fb)=> (59u<<26)|((uint)fd<<21)|((uint)fa<<16)|((uint)fb<<11)|(21u<<1);
static uint Fdivs(int fd,int fa,int fb)=> (59u<<26)|((uint)fd<<21)|((uint)fa<<16)|((uint)fb<<11)|(18u<<1);
static uint Mtfsb1(int bit)=>(63u<<26)|((uint)bit<<21)|(38u<<1);
static uint Fctiw(int fd,int fb,bool towardZero)=>(63u<<26)|((uint)fd<<21)|((uint)fb<<11)|((towardZero?15u:14u)<<1);
static uint Fmadds(int fd,int fa,int fc,int fb)=>(59u<<26)|((uint)fd<<21)|((uint)fa<<16)|((uint)fb<<11)|((uint)fc<<6)|(29u<<1);
static uint Fres(int fd,int fb)=>(59u<<26)|((uint)fd<<21)|((uint)fb<<11)|(24u<<1);
static uint Frsqrte(int fd,int fb)=>(63u<<26)|((uint)fd<<21)|((uint)fb<<11)|(26u<<1);
static uint PsqL(int fd,int ra,int d)=>(56u<<26)|((uint)fd<<21)|((uint)ra<<16)|(uint)(d&0xfff);
static uint PsqSt(int fs,int ra,int d)=>(60u<<26)|((uint)fs<<21)|((uint)ra<<16)|(uint)(d&0xfff);
static uint PsAdd(int fd,int fa,int fb)=>(4u<<26)|((uint)fd<<21)|((uint)fa<<16)|((uint)fb<<11)|(21u<<1);
static uint PsRes(int fd,int fb)=>(4u<<26)|((uint)fd<<21)|((uint)fb<<11)|(24u<<1);
static uint PsRsqrte(int fd,int fb)=>(4u<<26)|((uint)fd<<21)|((uint)fb<<11)|(26u<<1);
static uint Fcmp(int crfd,int fa,int fb,bool ordered)=>(63u<<26)|((uint)crfd<<23)|((uint)fa<<16)|((uint)fb<<11)|((ordered?32u:0u)<<1);
static uint PsCmp0(int crfd,int fa,int fb,bool ordered)=>(4u<<26)|((uint)crfd<<23)|((uint)fa<<16)|((uint)fb<<11)|((ordered?32u:0u)<<1);
static uint PsNeg(int fd,int fb)=>(4u<<26)|((uint)fd<<21)|((uint)fb<<11)|(40u<<1);

var words=new uint[] {
  Lis(3,output),Ori(3,3,output),Addi(4,0,0x7fff),Add(5,4,4),Stw(5,3,0),
  Lis(6,data),Ori(6,6,data),Lfs(1,6,0),Lfs(2,6,4),Fadds(3,1,2),Stfs(3,3,4),
  PsqL(4,6,8),PsqL(5,6,16),PsAdd(6,4,5),PsqSt(6,3,8),
  Lfs(7,6,24),Fdivs(8,1,7),Stfs(8,3,16),
  Lfs(9,6,28),Lfs(10,6,32),Fadds(11,9,10),Stfs(11,3,20),
  Mtfsb1(24),Lfs(12,6,36),Fadds(12,9,10),Stfs(12,3,24),
  Lfs(13,6,40),Fctiw(14,13,true),Fctiw(15,13,false),Fctiw(13,9,false),
  Lfs(16,6,36),Fmadds(16,9,7,1),
  Mtfsb1(27),Lfs(17,6,36),Fres(17,7),
  Lfs(18,6,36),Lfs(19,6,12),Frsqrte(18,19),
  PsqL(20,6,24),PsRes(21,20),PsRsqrte(22,4),
  PsqL(24,6,28),PsNeg(25,24),PsAdd(26,24,25),
  Lfs(23,6,44),Fcmp(3,23,1,true),Fcmp(4,23,1,false),
  PsCmp0(5,23,1,true),PsCmp0(6,23,1,false),
  0x4e800020u
};
const int header=0x100;
var codeBytes=words.Length*4;
var image=new byte[header+codeBytes+48];
BinaryPrimitives.WriteUInt32BigEndian(image.AsSpan(0x00,4),(uint)header);
BinaryPrimitives.WriteUInt32BigEndian(image.AsSpan(0x1c,4),(uint)(header+codeBytes));
BinaryPrimitives.WriteUInt32BigEndian(image.AsSpan(0x48,4),entry);
BinaryPrimitives.WriteUInt32BigEndian(image.AsSpan(0x64,4),data);
BinaryPrimitives.WriteUInt32BigEndian(image.AsSpan(0x90,4),(uint)codeBytes);
BinaryPrimitives.WriteUInt32BigEndian(image.AsSpan(0xac,4),48u);
BinaryPrimitives.WriteUInt32BigEndian(image.AsSpan(0xe0,4),entry);
for(int i=0;i<words.Length;++i) BinaryPrimitives.WriteUInt32BigEndian(image.AsSpan(header+i*4,4),words[i]);
BinaryPrimitives.WriteUInt32BigEndian(image.AsSpan(header+codeBytes,4),0x3fc00000u); // 1.5
BinaryPrimitives.WriteUInt32BigEndian(image.AsSpan(header+codeBytes+4,4),0x40100000u); // 2.25
BinaryPrimitives.WriteUInt32BigEndian(image.AsSpan(header+codeBytes+8,4),0x3fc00000u); // {1.5,
BinaryPrimitives.WriteUInt32BigEndian(image.AsSpan(header+codeBytes+12,4),0xc0000000u); // -2.0}
BinaryPrimitives.WriteUInt32BigEndian(image.AsSpan(header+codeBytes+16,4),0x40200000u); // {2.5,
BinaryPrimitives.WriteUInt32BigEndian(image.AsSpan(header+codeBytes+20,4),0x40800000u); // 4.0}
BinaryPrimitives.WriteUInt32BigEndian(image.AsSpan(header+codeBytes+24,4),0u);
BinaryPrimitives.WriteUInt32BigEndian(image.AsSpan(header+codeBytes+28,4),0x7f800000u); // +inf
BinaryPrimitives.WriteUInt32BigEndian(image.AsSpan(header+codeBytes+32,4),0xff800000u); // -inf
BinaryPrimitives.WriteUInt32BigEndian(image.AsSpan(header+codeBytes+36,4),0x42280000u); // 42.0
BinaryPrimitives.WriteUInt32BigEndian(image.AsSpan(header+codeBytes+40,4),0x40300000u); // 2.75
BinaryPrimitives.WriteUInt32BigEndian(image.AsSpan(header+codeBytes+44,4),0x7fc00042u); // qNaN
var path=Path.GetFullPath(args[0]); Directory.CreateDirectory(Path.GetDirectoryName(path)!); File.WriteAllBytes(path,image);
Console.WriteLine($"semanticFixture={path} entry=0x{entry:X8} bytes={image.Length}");
return 0;
