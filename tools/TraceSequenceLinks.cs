using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Text;

internal static class TraceSequenceLinks
{
    private sealed class Export
    {
        public int Index;
        public long Offset;
        public int Size;
        public string Path;
        public string ClassName;
    }

    private sealed class Link
    {
        public int ObjectIndex;
        public int InputIndex;
    }

    private static readonly Dictionary<int, string> Names = new Dictionary<int, string>();
    private static readonly Dictionary<int, Export> Exports = new Dictionary<int, Export>();
    private static readonly Dictionary<string, Export> ExportsByPath =
        new Dictionary<string, Export>(StringComparer.OrdinalIgnoreCase);

    private static string ReadName(BinaryReader reader)
    {
        int index = reader.ReadInt32();
        reader.ReadInt32();
        string name;
        return Names.TryGetValue(index, out name) ? name : "#" + index;
    }

    private static string ReadFString(BinaryReader reader, int size)
    {
        long start = reader.BaseStream.Position;
        int count = reader.ReadInt32();
        string value = "";
        if (count > 0 && count <= size - 4)
            value = Encoding.GetEncoding(1252).GetString(reader.ReadBytes(count)).TrimEnd('\0');
        else if (count < 0 && -count * 2 <= size - 4)
            value = Encoding.Unicode.GetString(reader.ReadBytes(-count * 2)).TrimEnd('\0');
        reader.BaseStream.Position = start + size;
        return value;
    }

    private static List<Link> ReadLinksArray(BinaryReader reader, int size)
    {
        long end = reader.BaseStream.Position + size;
        var links = new List<Link>();
        int count = reader.ReadInt32();
        for (int item = 0; item < count && reader.BaseStream.Position < end; ++item)
        {
            var link = new Link { ObjectIndex = 0, InputIndex = -1 };
            while (reader.BaseStream.Position + 8 <= end)
            {
                string property = ReadName(reader);
                if (property == "None") break;
                string type = ReadName(reader);
                int propertySize = reader.ReadInt32();
                reader.ReadInt32();
                if (type == "StructProperty" || type == "ByteProperty") ReadName(reader);
                if (type == "BoolProperty")
                {
                    reader.ReadByte();
                    continue;
                }
                long valueStart = reader.BaseStream.Position;
                if (property == "LinkedOp" && type == "ObjectProperty" && propertySize == 4)
                    link.ObjectIndex = reader.ReadInt32();
                else if (property == "InputLinkIdx" && type == "IntProperty" && propertySize == 4)
                    link.InputIndex = reader.ReadInt32();
                reader.BaseStream.Position = valueStart + propertySize;
            }
            links.Add(link);
        }
        reader.BaseStream.Position = end;
        return links;
    }

    private static void ReadOutputLinks(BinaryReader reader, int size, TextWriter writer, Export source)
    {
        long end = reader.BaseStream.Position + size;
        int count = reader.ReadInt32();
        for (int outputIndex = 0; outputIndex < count && reader.BaseStream.Position < end; ++outputIndex)
        {
            string description = "";
            float delay = 0.0f;
            var links = new List<Link>();
            while (reader.BaseStream.Position + 8 <= end)
            {
                string property = ReadName(reader);
                if (property == "None") break;
                string type = ReadName(reader);
                int propertySize = reader.ReadInt32();
                reader.ReadInt32();
                if (type == "StructProperty" || type == "ByteProperty") ReadName(reader);
                if (type == "BoolProperty")
                {
                    reader.ReadByte();
                    continue;
                }
                long valueStart = reader.BaseStream.Position;
                if (property == "Links" && type == "ArrayProperty")
                    links = ReadLinksArray(reader, propertySize);
                else if (property == "LinkDesc" && type == "StrProperty")
                    description = ReadFString(reader, propertySize);
                else if (property == "ActivateDelay" && type == "FloatProperty" && propertySize == 4)
                    delay = reader.ReadSingle();
                reader.BaseStream.Position = valueStart + propertySize;
            }
            foreach (Link link in links)
            {
                Export target;
                string targetText = link.ObjectIndex > 0 && Exports.TryGetValue(link.ObjectIndex - 1, out target)
                    ? target.Path + " [" + target.ClassName + "]"
                    : "object-index " + link.ObjectIndex;
                writer.WriteLine(source.Path + " [" + source.ClassName + "] --" + outputIndex +
                    ":" + description + " delay=" + delay.ToString("0.######", CultureInfo.InvariantCulture) +
                    "--> " + targetText + " input=" + link.InputIndex);
            }
        }
        reader.BaseStream.Position = end;
    }

    private static int Main(string[] args)
    {
        if (args.Length != 3)
        {
            Console.Error.WriteLine("usage: TraceSequenceLinks <object-dump.txt> <package> <output.txt>");
            return 2;
        }
        string[] lines = File.ReadAllLines(args[0]);
        bool names = false;
        foreach (string line in lines)
        {
            if (line.StartsWith("NAMES\t")) { names = true; continue; }
            if (line == "RELEVANT OBJECTS") { names = false; continue; }
            string[] fields = line.Split('\t');
            int index;
            if (!int.TryParse(fields[0], out index)) continue;
            if (names)
            {
                if (fields.Length >= 2) Names[index] = fields[1];
            }
            else if (fields.Length >= 9)
            {
                long offset;
                int size;
                if (!long.TryParse(fields[4], out offset) || !int.TryParse(fields[5], out size)) continue;
                var export = new Export { Index = index, Offset = offset, Size = size,
                    Path = fields[6], ClassName = fields[8] };
                Exports[index] = export;
                ExportsByPath[export.Path] = export;
            }
        }

        using (var stream = File.OpenRead(args[1]))
        using (var reader = new BinaryReader(stream))
        using (var writer = new StreamWriter(args[2], false))
        {
            string currentPath = null;
            int outputSize = -1;
            foreach (string line in lines)
            {
                if (line.StartsWith("// ===== ") && line.EndsWith(" ====="))
                {
                    currentPath = line.Substring(9, line.Length - 15);
                    outputSize = -1;
                    continue;
                }
                if (currentPath == null) continue;
                if (line.StartsWith("// OutputLinks\tArrayProperty\t"))
                {
                    string[] fields = line.Substring(3).Split('\t');
                    int.TryParse(fields[2], out outputSize);
                    continue;
                }
                if (outputSize >= 0 && line.StartsWith("//   <_PropertyValuePosition>k__BackingField="))
                {
                    long relative;
                    if (!long.TryParse(line.Substring(line.IndexOf('=') + 1), out relative)) continue;
                    Export source;
                    if (!ExportsByPath.TryGetValue(currentPath, out source)) continue;
                    reader.BaseStream.Position = source.Offset + relative;
                    ReadOutputLinks(reader, outputSize, writer, source);
                    outputSize = -1;
                }
            }
        }
        return 0;
    }
}
