using System;
using System.IO;
using System.Linq;
using UELib;
using UELib.Core;

internal static class DumpKidFunctions
{
    private static int Main(string[] args)
    {
        if (args.Length != 2)
        {
            Console.Error.WriteLine("usage: DumpKidFunctions <package.u> <output.txt>");
            return 2;
        }

        try
        {
            using (var package = UnrealLoader.LoadPackage(args[0]))
            using (var writer = new StreamWriter(args[1], false))
            {
                package.InitializePackage(UnrealPackage.InitFlags.Construct |
                                          UnrealPackage.InitFlags.RegisterClasses);

                var functions = package.Objects.OfType<UFunction>().ToList();
                Console.WriteLine("Functions: " + functions.Count);
                var dumped = 0;
                var failed = 0;
                foreach (var function in functions)
                {
                    try
                    {
                        function.BeginDeserializing();
                        writer.WriteLine("// ===== " + function.GetPath() + " =====");
                        writer.WriteLine(function.Decompile());
                        writer.WriteLine();
                        ++dumped;
                    }
                    catch (Exception exception)
                    {
                        writer.WriteLine("// ===== " + function.GetPath() + " =====");
                        writer.WriteLine("// DECOMPILE ERROR: " + exception.Message);
                        writer.WriteLine();
                        ++failed;
                    }
                }

                Console.WriteLine("Dumped: " + dumped + "; failed: " + failed);
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
