using System;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Text;
using UELib;
using UELib.Core;

internal static class DumpKidPackageObjects
{
    private static object ReadMember(object instance, string name)
    {
        if (instance == null) return null;
        var type = instance.GetType();
        var field = type.GetField(name, BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Instance);
        if (field != null) return field.GetValue(instance);
        var property = type.GetProperties(BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Instance)
                           .FirstOrDefault(item => item.Name == name && item.GetIndexParameters().Length == 0);
        return property != null ? property.GetValue(instance, null) : null;
    }

    private static string Text(object value)
    {
        return value == null ? "<null>" : value.ToString().Replace('\t', ' ');
    }

    private static string TargetText;

    private static string RawPropertyBytes(UExportTableItem export, UDefaultProperty property)
    {
        try
        {
            var valuePosition = Convert.ToInt64(ReadMember(property, "_PropertyValuePosition"));
            var buffer = ReadMember(property, "_Buffer") as UObjectStream;
            if (buffer == null || property.Size < 0 || property.Size > 1024 * 1024)
                return "<unavailable>";
            var savedPosition = buffer.Position;
            buffer.Position = valuePosition;
            var bytes = new byte[property.Size];
            var read = buffer.Read(bytes, 0, bytes.Length);
            buffer.Position = savedPosition;
            if (read != bytes.Length) return "<short read>";
            return BitConverter.ToString(bytes).Replace("-", " ");
        }
        catch (Exception exception)
        {
            return "<" + exception.GetType().Name + ">";
        }
    }

    private static bool IsTargetObject(UExportTableItem export)
    {
        var obj = export.Object;
        if (!string.IsNullOrEmpty(TargetText))
        {
            if (!TargetText.StartsWith("property:", StringComparison.OrdinalIgnoreCase))
            {
                return export.GetPath().IndexOf(TargetText, StringComparison.OrdinalIgnoreCase) >= 0;
            }
            if (obj == null) return false;
            try
            {
                obj.BeginDeserializing();
                var needle = TargetText.Substring("property:".Length);
                return obj.Properties.Any(property =>
                    property.Name.ToString().IndexOf(needle, StringComparison.OrdinalIgnoreCase) >= 0 ||
                    Text(property.Value).IndexOf(needle, StringComparison.OrdinalIgnoreCase) >= 0);
            }
            catch
            {
                return false;
            }
        }
        if (obj == null || obj.Class == null || obj.Class.Name != "AnimSequence") return false;
        try
        {
            obj.BeginDeserializing();
            return obj.Properties.Any(property => property.Name.ToString() == "SequenceName" &&
                (Text(property.Value).IndexOf("ANI_CH_EVE_Intro", StringComparison.OrdinalIgnoreCase) >= 0 ||
                 Text(property.Value).IndexOf("ANI_CH_ATK_Koharu_Intro", StringComparison.OrdinalIgnoreCase) >= 0));
        }
        catch
        {
            return false;
        }
    }

    private static int Main(string[] args)
    {
        Encoding.RegisterProvider(CodePagesEncodingProvider.Instance);
        if (args.Length < 2 || args.Length > 3)
        {
            Console.Error.WriteLine("usage: DumpKidPackageObjects <package> <output.txt> [path-substring]");
            return 2;
        }

        TargetText = args.Length == 3 ? args[2] : null;

        try
        {
            using (var package = UnrealLoader.LoadPackage(args[0]))
            using (var writer = new StreamWriter(args[1], false))
            {
                writer.WriteLine("PACKAGE MEMBERS");
                for (var type = package.GetType(); type != null; type = type.BaseType)
                {
                    foreach (var field in type.GetFields(BindingFlags.Public | BindingFlags.NonPublic |
                                                         BindingFlags.Instance | BindingFlags.DeclaredOnly))
                    {
                        writer.WriteLine("FIELD\t" + type.FullName + "\t" + field.Name + "\t" +
                                         field.FieldType.FullName);
                    }
                    foreach (var property in type.GetProperties(BindingFlags.Public | BindingFlags.NonPublic |
                                                                BindingFlags.Instance | BindingFlags.DeclaredOnly))
                    {
                        writer.WriteLine("PROPERTY\t" + type.FullName + "\t" + property.Name + "\t" +
                                         property.PropertyType.FullName);
                    }
                }
                writer.WriteLine();

                try
                {
                    package.InitializePackage(UnrealPackage.InitFlags.All);
                }
                catch (Exception exception)
                {
                    writer.WriteLine("INITIALIZE ERROR: " + exception);
                }

                writer.WriteLine("EXPORTS\t" + package.Exports.Count);
                foreach (var export in package.Exports)
                {
                    var obj = export.Object;
                    var classIndex = ReadMember(export, "ClassIndex");
                    var outerIndex = ReadMember(export, "OuterIndex");
                    var objectName = ReadMember(export, "ObjectName");
                    writer.WriteLine(string.Join("\t", new[] {
                        Text(export.Index),
                        Text(objectName),
                        Text(classIndex),
                        Text(outerIndex),
                        Text(export.SerialOffset),
                        Text(export.SerialSize),
                        export.GetPath(),
                        obj == null ? "<no object>" : obj.GetType().FullName,
                        obj == null || obj.Class == null ? "<no class>" : obj.Class.Name
                    }));
                }

                writer.WriteLine();
                writer.WriteLine("NAMES\t" + package.Names.Count);
                foreach (var name in package.Names)
                {
                    writer.WriteLine(name.Index + "\t" + name.Name);
                }

                writer.WriteLine();
                writer.WriteLine("RELEVANT OBJECTS");
                foreach (var export in package.Exports.Where(item =>
                    IsTargetObject(item)))
                {
                    writer.WriteLine("// ===== " + export.GetPath() + " =====");
                    var obj = export.Object;
                    if (obj == null)
                    {
                        writer.WriteLine("// no constructed object");
                        continue;
                    }
                    try
                    {
                        obj.BeginDeserializing();
                        writer.WriteLine("// PROPERTIES");
                        foreach (var property in obj.Properties)
                        {
                            writer.WriteLine("// " + property.Name + "\t" + property.Type + "\t" +
                                             property.Size + "\t" + property.ArrayIndex + "\t" +
                                             Text(property.Value));
                            for (var propertyType = property.GetType(); propertyType != null;
                                 propertyType = propertyType.BaseType)
                            {
                                foreach (var field in propertyType.GetFields(BindingFlags.Public |
                                    BindingFlags.NonPublic | BindingFlags.Instance |
                                    BindingFlags.DeclaredOnly))
                                {
                                    writer.WriteLine("//   " + field.Name + "=" +
                                                     Text(field.GetValue(property)));
                                }
                            }
                            if (property.Type.ToString() == "ArrayProperty")
                            {
                                writer.WriteLine("//   RawBytes=" + RawPropertyBytes(export, property));
                            }
                        }
                        writer.WriteLine(obj.Decompile());
                    }
                    catch (Exception exception)
                    {
                        writer.WriteLine("// DECOMPILE ERROR: " + exception);
                    }
                }
            }
            return 0;
        }
        catch (Exception exception)
        {
            Console.Error.WriteLine(exception);
            return 1;
        }
    }
}
