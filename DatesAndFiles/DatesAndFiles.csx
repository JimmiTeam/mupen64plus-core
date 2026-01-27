
#r "nuget: SixLabors.ImageSharp, 3.1.12"
using SixLabors.ImageSharp;
using SixLabors.ImageSharp.Formats.Jpeg;
using SixLabors.ImageSharp.Processing;
using System.IO.Compression;

var directoryRaw = Directory.EnumerateDirectories(".").Select(d => new DirectoryInfo(d).Name)
                    .OrderByDescending(d => new DirectoryInfo(d).CreationTime).ToList()[0];
var images = Directory.EnumerateFiles("./images").Where(f => f.EndsWith(".png")).ToList();

// var directory = "not-a-date";

var directory = directoryRaw.Replace(".", ":");

DateTime dateTime;
if (DateTime.TryParse(directory, out dateTime))
{
    Console.WriteLine(dateTime);
}
else
{
    Console.WriteLine(directory);
}

File.Delete($"compressed.zip");

var tempFolder = "temp_thumbnails";
Directory.CreateDirectory(tempFolder);

foreach (var image in images)
{
    var imageName = $"{tempFolder}/{new FileInfo(image).Name}";
    var loadedImage = Image.Load(image);
    var encoder = new JpegEncoder
    {
        Quality = 40,
    };
    loadedImage.Mutate(x => x.Resize(320, 240));

    loadedImage.Save(imageName, encoder);
}

using (ZipArchive zip = ZipFile.Open($"compressed.zip", ZipArchiveMode.Create))
{
    foreach (var image in images)
    {
        var imageName = new FileInfo(image).Name;
        zip.CreateEntryFromFile($"{tempFolder}/{new FileInfo(image).Name}", imageName, CompressionLevel.Optimal);
        File.Delete($"{tempFolder}/{new FileInfo(image).Name}");
        Directory.Delete(tempFolder);
        zip.CreateEntryFromFile("text.txt", "text", CompressionLevel.Optimal);
        zip.CreateEntryFromFile("text2.txt", "text2", CompressionLevel.Optimal);
    }
}

// using (ZipArchive zip = ZipFile.Open($"{directoryRaw}.zip", ZipArchiveMode.Read))
// {
//     zip.GetEntry("text").ExtractToFile("text2.txt");
//     string contents = File.ReadAllText("text2.txt");

//     Console.WriteLine(contents);
// }

// // File.Move("text2.txt", "text3.txt");
// File.Delete("test2.txt");
