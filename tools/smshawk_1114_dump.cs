// SMSHawk 1.11.4 reference stream: RGB565 screen followed by 8 KiB WRAM.
// Compiled against the unmodified historical sources, outside the repository.
using System;
using System.IO;
using BizHawk.Emulation.Common;
using BizHawk.Emulation.Cores.Sega.MasterSystem;

sealed class MoviePad : IController {
    public byte Mask;
    static readonly string[] Keys = { "P1 Up", "P1 Down", "P1 Left", "P1 Right", "P1 B1", "P1 B2", "Pause" };
    public ControllerDefinition Type { get { return SMS.SmsController; } }
    public bool this[string key] { get { for (int i = 0; i < Keys.Length; i++) if (key == Keys[i]) return (Mask & (1 << i)) != 0; return false; } }
    public bool IsPressed(string key) { return this[key]; }
    public float GetFloat(string key) { return 0; }
}
static class LegacyDump {
    static void Main(string[] args) {
        if (args.Length != 3) throw new ArgumentException("ROM masks.bin frame-stream");
        var masks = File.ReadAllBytes(args[1]);
        var core = new SMS(new CoreComm(Console.Error.WriteLine, Console.Error.WriteLine), GameInfo.NullInstance, File.ReadAllBytes(args[0]), null, null);
        var pad = new MoviePad(); core.Controller = pad;
        var pixels = new byte[256 * 192 * 2];
        using (var stream = new FileStream(args[2], FileMode.Create, FileAccess.Write)) {
            for (int frame = 0; frame < masks.Length; frame++) {
                pad.Mask = masks[frame]; core.FrameAdvance(true, false);
                var video = core.Vdp.GetVideoBuffer();
                if (core.Vdp.BufferWidth != 256 || core.Vdp.BufferHeight != 192) throw new InvalidOperationException("expected SMS 256x192 framebuffer");
                for (int i = 0; i < video.Length; i++) {
                    int c = video[i], rgb = ((c >> 19) & 31) << 11 | ((c >> 10) & 63) << 5 | ((c >> 3) & 31);
                    pixels[i * 2] = (byte)rgb; pixels[i * 2 + 1] = (byte)(rgb >> 8);
                }
                stream.Write(pixels, 0, pixels.Length); stream.Write(core.SystemRam, 0, core.SystemRam.Length);
            }
        }
    }
}
