using System;
using System.Collections;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Text;
using UELib;
using UELib.Core;

internal static class DumpKidFunctionBytes
{
    private static object ReadMember(object instance, string name)
    {
        if (instance == null) return null;
        for (var type = instance.GetType(); type != null; type = type.BaseType)
        {
            var field = type.GetField(name, BindingFlags.Public | BindingFlags.NonPublic |
                                            BindingFlags.Instance | BindingFlags.DeclaredOnly);
            if (field != null) return field.GetValue(instance);
            var property = type.GetProperties(BindingFlags.Public | BindingFlags.NonPublic |
                                               BindingFlags.Instance | BindingFlags.DeclaredOnly)
                               .FirstOrDefault(item => item.Name == name &&
                                                       item.GetIndexParameters().Length == 0);
            if (property != null) return property.GetValue(instance, null);
        }
        return null;
    }

    private static int Main(string[] args)
    {
        if (args.Length != 3)
        {
            Console.Error.WriteLine("usage: DumpKidFunctionBytes <package> <path-substring> <output.bin>");
            return 2;
        }

        try
        {
#if NET7_0
            Encoding.RegisterProvider(CodePagesEncodingProvider.Instance);
#endif
            using var package = UnrealLoader.LoadPackage(args[0]);
            package.InitializePackage(UnrealPackage.InitFlags.All);
            for (var index = 0; index < package.Names.Count; ++index)
            {
                var name = package.Names[index].ToString();
                if (name.Equals("AbsorbState", StringComparison.OrdinalIgnoreCase) ||
                    name.Equals("StunState", StringComparison.OrdinalIgnoreCase))
                    Console.WriteLine("NAME " + index + " = " + name);
            }
            var matches = package.Objects.OfType<UFunction>()
                .Where(function => function.GetPath().IndexOf(args[1], StringComparison.OrdinalIgnoreCase) >= 0)
                .ToList();
            if (matches.Count != 1)
            {
                Console.Error.WriteLine("matching functions: " + matches.Count);
                foreach (var match in matches) Console.Error.WriteLine(match.GetPath());
                return 1;
            }

            var function = matches[0];
            function.BeginDeserializing();
            Console.WriteLine(function.GetPath());
            for (var type = function.GetType(); type != null; type = type.BaseType)
            {
                foreach (var field in type.GetFields(BindingFlags.Public | BindingFlags.NonPublic |
                                                     BindingFlags.Instance | BindingFlags.DeclaredOnly))
                {
                    object value = field.GetValue(function);
                    string text;
                    try { text = value == null ? "<null>" : value.ToString(); }
                    catch { text = "<ToString failed>"; }
                    Console.WriteLine("FIELD " + type.FullName + "." + field.Name + " [" +
                                      field.FieldType.FullName + "] = " + text);
                }
                foreach (var property in type.GetProperties(BindingFlags.Public | BindingFlags.NonPublic |
                                                            BindingFlags.Instance | BindingFlags.DeclaredOnly)
                                             .Where(item => item.GetIndexParameters().Length == 0))
                {
                    object value;
                    try { value = property.GetValue(function, null); }
                    catch { continue; }
                    string text;
                    try { text = value == null ? "<null>" : value.ToString(); }
                    catch { text = "<ToString failed>"; }
                    Console.WriteLine("PROPERTY " + type.FullName + "." + property.Name + " [" +
                                      property.PropertyType.FullName + "] = " + text);
                }
            }

            var script = ReadMember(function, "Script");
            if (script is byte[] byteArray)
            {
                File.WriteAllBytes(args[2], byteArray);
                Console.WriteLine("wrote " + byteArray.Length + " bytes from Script byte[]");
                return 0;
            }
            if (script is IEnumerable enumerable)
            {
                var bytes = enumerable.Cast<object>().Select(Convert.ToByte).ToArray();
                File.WriteAllBytes(args[2], bytes);
                Console.WriteLine("wrote " + bytes.Length + " bytes from enumerable Script");
                return 0;
            }

            var scriptOffset = Convert.ToInt64(ReadMember(function, "ScriptOffset"));
            var byteScriptSize = Convert.ToInt32(ReadMember(function, "ByteScriptSize"));
            var serialOffset = function.ExportTable.SerialOffset;
            if (scriptOffset >= 0 && byteScriptSize > 0)
            {
                using var stream = File.OpenRead(args[0]);
                stream.Position = serialOffset + scriptOffset;
                var bytes = new byte[byteScriptSize];
                var read = stream.Read(bytes, 0, bytes.Length);
                if (read != bytes.Length)
                {
                    Console.Error.WriteLine("short raw script read: " + read + "/" + bytes.Length);
                    return 1;
                }
                File.WriteAllBytes(args[2], bytes);
                Console.WriteLine("wrote " + bytes.Length + " bytes from package offset " +
                                  (serialOffset + scriptOffset));
                return 0;
            }
            Console.Error.WriteLine("Script member unavailable and raw offsets invalid.");
            return 1;
        }
        catch (Exception exception)
        {
            Console.Error.WriteLine(exception);
            return 1;
        }
    }
}
