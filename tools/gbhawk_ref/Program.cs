using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using BizHawk.Emulation.Common;
using BizHawk.Emulation.Cores.Nintendo.GBHawk;

// GBHawk headless reference: replays a BK2/plain input log, writes one line per frame
// (PC SP AF BC DE HL, screen hash) so another emulator can be compared to the one that recorded the movie.
class Fw : ICoreFileProvider {
    public byte[] Bios;
    public string GetRetroSaveRAMDirectory(string p) => "/tmp";
    public string GetRetroSystemPath(string p) => "/tmp";
    public string GetUserPath(string s, bool t) => "/tmp";
    public byte[] GetFirmware(FirmwareID id, string msg = null) => Bios;
    public byte[] GetFirmwareOrThrow(FirmwareID id, string msg = null) => Bios ?? throw new Exception(msg);
    public (byte[] FW, GameInfo Game) GetFirmwareWithGameInfoOrThrow(FirmwareID id, string msg = null) => (Bios ?? throw new Exception(msg), GameInfo.NullInstance);
}
class Pad : IController {
    public ControllerDefinition Definition { get; set; }
    public HashSet<string> Down = new();
    public IReadOnlyCollection<(string Name, int Strength)> GetHapticsSnapshot() => Array.Empty<(string, int)>();
    public bool IsPressed(string b) => Down.Contains(b);
    public int AxisValue(string n) => 0;
    public void SetHapticChannelStrength(string n, int s) { }
}
static class P {
    static int Main(string[] a) {
        if (a.Length < 4) { Console.Error.WriteLine("gbh ROM inputlog.txt out.txt BIOS [shotprefix every]"); return 2; }
        var rom = File.ReadAllBytes(a[0]);
        var keys = new[] { "Up", "Down", "Left", "Right", "Start", "Select", "B", "A" };
        var frames = new List<string>();
        foreach (var l in File.ReadLines(a[1])) if (l.StartsWith("|")) frames.Add(l.Substring(1));
        var bios = File.ReadAllBytes(a[3]);
        var sha = Convert.ToHexString(System.Security.Cryptography.SHA1.HashData(bios));
        Console.Error.WriteLine("BIOS SHA-1 " + sha + (sha == "4ED31EC6B0B175BB109C0EB5FD3D193DA823339F" ? " (the movie's DMG boot ROM)" : " (NOT the movie's boot ROM)"));
        var comm = new CoreComm(s => { }, (s, d) => { }, new Fw { Bios = bios }, CoreComm.CorePreferencesFlags.None);
        var ss = new GBHawk.GBSyncSettings { ConsoleMode = GBHawk.GBSyncSettings.ConsoleModeType.GB, GBACGB = true, Use_SRAM = false };
        var gb = new GBHawk(comm, GameInfo.NullInstance, rom, new GBHawk.GBSettings(), ss);
        var pad = new Pad { Definition = gb.ControllerDefinition };
        using var o = new StreamWriter(a[2]);
        string shot = a.Length > 4 ? a[4] : null; int every = a.Length > 5 ? int.Parse(a[5]) : 0;
        for (int f = 0; f < frames.Count; f++) {
            pad.Down.Clear();
            for (int k = 0; k < 8; k++) if (frames[f][k] != '.') pad.Down.Add("P1 " + keys[k]);
            gb.FrameAdvance(pad, true, false);
            var r = gb.GetCpuFlagsAndRegisters();
            uint h = 2166136261; foreach (int px in gb.GetVideoBuffer()) h = (h ^ (uint)px) * 16777619;
            o.WriteLine($"{f + 1} {r["PC"].Value:x4} {r["SP"].Value:x4} {r["AF"].Value:x4} {r["BC"].Value:x4} {r["DE"].Value:x4} {r["HL"].Value:x4} {h:x8}");
            if (shot != null && every > 0 && (f + 1) % every == 0) {
                var vb = gb.GetVideoBuffer();
                using var w = new BinaryWriter(File.Create($"{shot}{f + 1:D6}.ppm"));
                w.Write(System.Text.Encoding.ASCII.GetBytes("P6 160 144 255\n"));
                foreach (int px in vb) { w.Write((byte)(px >> 16)); w.Write((byte)(px >> 8)); w.Write((byte)px); }
            }
        }
        return 0;
    }
}
