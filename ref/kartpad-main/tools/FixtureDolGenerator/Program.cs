using System.Buffers.Binary;

if (args.Length != 1)
{
    Console.Error.WriteLine("usage: FixtureDolGenerator <output.dol>");
    return 2;
}

const uint entryPoint = 0x80001000;
const uint commandAddress = 0x80010000;
const uint magic = 0x4B504758; // KPGX — KartPad translated GX fixture
const uint clearRgba = 0x102030FF;
const uint vertexCount = 3;
var payload = new uint[]
{
    magic,
    clearRgba,
    vertexCount,
    0x47583031, // GX01 — payload version
    0xBF400000, 0xBF400000, 0x00000000, 0x000000FF, // left
    0x00000000, 0x3F400000, 0x00000000, 0x000000FF, // top
    0x3F400000, 0xBF400000, 0x00000000, 0x000000FF, // right
};

static uint Lis(int destination, uint value) =>
    0x3C000000u | ((uint)destination << 21) | ((value >> 16) & 0xFFFFu);
static uint Ori(int destination, int source, uint value) =>
    0x60000000u | ((uint)source << 21) | ((uint)destination << 16) | (value & 0xFFFFu);
static uint Stw(int source, int address, int offset) =>
    0x90000000u | ((uint)source << 21) | ((uint)address << 16) | (uint)(offset & 0xFFFF);

var words = new List<uint>
{
    Lis(3, commandAddress),
    Ori(3, 3, commandAddress),
};
for (var index = 0; index < payload.Length; ++index)
{
    var value = payload[index];
    words.Add(Lis(4, value));
    words.Add(Ori(4, 4, value));
    words.Add(Stw(4, 3, index * sizeof(uint)));
}
words.Add(0x4E800020u); // blr

const int headerSize = 0x100;
var image = new byte[headerSize + words.Count * sizeof(uint)];
BinaryPrimitives.WriteUInt32BigEndian(image.AsSpan(0x00, 4), headerSize);
BinaryPrimitives.WriteUInt32BigEndian(image.AsSpan(0x48, 4), entryPoint);
BinaryPrimitives.WriteUInt32BigEndian(image.AsSpan(0x90, 4), (uint)(words.Count * sizeof(uint)));
BinaryPrimitives.WriteUInt32BigEndian(image.AsSpan(0xE0, 4), entryPoint);
for (var index = 0; index < words.Count; ++index)
{
    BinaryPrimitives.WriteUInt32BigEndian(image.AsSpan(headerSize + index * 4, 4), words[index]);
}

var output = Path.GetFullPath(args[0]);
Directory.CreateDirectory(Path.GetDirectoryName(output)!);
File.WriteAllBytes(output, image);
Console.WriteLine($"fixtureDol={output} entry=0x{entryPoint:X8} bytes={image.Length}");
return 0;
