using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Security.AccessControl;
using System.Security.Principal;
using Microsoft.Win32;

namespace Mikraf;

[StructLayout(LayoutKind.Sequential, Pack = 4)]
struct ApoPayload
{
    public int Bypass;
    public int SuppressDb;
    public int Method;
    public float ExpRatio;
    public float ExpThreshold;
    public float ExpOutput;
    public int ExpAttack;
    public int ExpRelease;
    public int ExpDetector;
    public float EqLow;
    public float EqMid;
    public float EqHigh;
    public float CompRatio;
    public float CompThreshold;
    public float CompOutput;
    public int CompAttack;
    public int CompRelease;
    public float LimThreshold;
    public int LimRelease;
    public float GainDb;
    public float GateOpen;
    public float GateClose;
    public int GateAttack;
    public int GateHold;
    public int GateRelease;
    public float UpRatio;
    public float UpThreshold;
    public float UpOutput;
    public int UpKnee;
    public int UpAttack;
    public int UpRelease;
    public int UpDetector;
    public int NsOn;
    public int GateOn;
    public int InvertOn;
    public int GainOn;
    public int ExpOn;
    public int UpOn;
    public int EqOn;
    public int CompOn;
    public int LimOn;
    public int ChainCount;
    public int C0, C1, C2, C3, C4, C5, C6, C7, C8, C9, C10, C11;
}

static class ApoBridge
{
    const int PayloadBytes = 216;
    static readonly object Gate = new();
    static bool _missing;
    static int _lastCount = -1;

    public static string ProgramDataDir => Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.CommonApplicationData), "Mikraf");

    public static void Push(RackSettings rack)
    {
        if (_missing)
            return;
        try
        {
            var payload = FromRack(rack);
            Directory.CreateDirectory(ProgramDataDir);
            TryShare(ProgramDataDir);
            MikrafApoPush(ref payload);
        }
        catch (DllNotFoundException)
        {
            _missing = true;
        }
        catch
        {
            // The capture APO reads the last file it can open. A failed push leaves the mic on its previous chain.
        }
    }

    public static string LiveText()
    {
        try
        {
            int count = 0, rate = 0, channels = 0;
            int live = MikrafApoLive(ref count, ref rate, ref channels);
            return $"live {live} count {count} rate {rate} ch {channels}";
        }
        catch (Exception ex)
        {
            return ex.GetType().Name;
        }
    }

    public static string Describe(string? deviceId)
    {
        if (_missing || !File.Exists(Path.Combine(AppContext.BaseDirectory, "mikraf-apo.dll")))
            return "APO dosyası yok. Mikraf klasöründe mikraf-apo.dll durmalı.";
        try
        {
            int installed = string.IsNullOrWhiteSpace(deviceId) ? 0 : MikrafApoQuery(deviceId);
            int count = 0, rate = 0, channels = 0;
            int live = MikrafApoLive(ref count, ref rate, ref channels);
            if (installed == 1)
            {
                bool moving = live == 1 && rate > 0 && count > 0 && (count != _lastCount || _lastCount < 0);
                _lastCount = live == 1 ? count : _lastCount;
                if (moving)
                    return $"Hazır, bu mikrofonda ({rate} Hz). Diğer programlar aynı mikrofonu seçsin.";
                return "Bu mikrofona yazıldı. Bir program mikrofonu açınca efekt orada işler.";
            }
            _lastCount = live == 1 ? count : -1;
            string? other = ReadDeviceFile();
            if (!string.IsNullOrWhiteSpace(other) && !string.Equals(other, deviceId, StringComparison.OrdinalIgnoreCase))
                return "Şu an başka bir mikrofonda. Bu aygıt için Uygula.";
            return "Henüz uygulanmadı. Uygula, seçili mikrofonun kendisine yazar.";
        }
        catch (DllNotFoundException)
        {
            _missing = true;
            return "Efekt dosyası yok.";
        }
    }

    public static bool VoiceClarityPresent(string? deviceId)
    {
        try
        {
            using var hive = RegistryKey.OpenBaseKey(RegistryHive.LocalMachine, RegistryView.Registry64);
            if (EndpointHasVoiceLink(hive, deviceId))
                return true;
            using var capture = hive.OpenSubKey(@"SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Capture");
            if (capture != null)
            {
                foreach (string name in capture.GetSubKeyNames())
                {
                    using var fx = capture.OpenSubKey(name + @"\FxProperties");
                    if (fx != null && HasVoiceLink(fx))
                        return true;
                }
            }
            using var swc = hive.OpenSubKey(@"SYSTEM\CurrentControlSet\Enum\SWC");
            if (swc != null)
            {
                foreach (string name in swc.GetSubKeyNames())
                {
                    if (name.Contains("VOICECLARITY", StringComparison.OrdinalIgnoreCase))
                        return true;
                }
            }
            using var cls = hive.OpenSubKey(@"SYSTEM\CurrentControlSet\Control\Class\{5989fce8-9cd0-467d-8a6a-5419e31529d4}");
            if (cls != null)
            {
                foreach (string name in cls.GetSubKeyNames())
                {
                    if (name.Length != 4 || !int.TryParse(name, out _))
                        continue;
                    using var inst = cls.OpenSubKey(name);
                    string blob = ((inst?.GetValue("InfPath") as string) ?? "") + " " + ((inst?.GetValue("DriverDesc") as string) ?? "") + " " + ((inst?.GetValue("MatchingDeviceId") as string) ?? "");
                    if (blob.Contains("voiceclarity", StringComparison.OrdinalIgnoreCase) || blob.Contains("Voice Clarity", StringComparison.OrdinalIgnoreCase))
                        return true;
                }
            }
        }
        catch
        {
            // The elevated installer checks the driver store itself.
        }
        return false;
    }

    public static bool VoiceClarityWasRemoved()
    {
        return File.Exists(Path.Combine(ProgramDataDir, "vc-driver-removed.txt"));
    }

    static bool EndpointHasVoiceLink(RegistryKey hive, string? deviceId)
    {
        string? key = EndpointKey(deviceId);
        if (key == null)
            return false;
        using var fx = hive.OpenSubKey($@"SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Capture\{key}\FxProperties");
        return fx != null && HasVoiceLink(fx);
    }

    static bool HasVoiceLink(RegistryKey fx)
    {
        foreach (string name in fx.GetValueNames())
        {
            if (name.Contains("9e6136e0", StringComparison.OrdinalIgnoreCase))
                return true;
        }
        return false;
    }

    static string? EndpointKey(string? deviceId)
    {
        if (string.IsNullOrWhiteSpace(deviceId))
            return null;
        int start = deviceId.LastIndexOf('{');
        int end = deviceId.LastIndexOf('}');
        if (start < 0 || end <= start)
            return null;
        return deviceId[start..(end + 1)];
    }

    public static string Install(string deviceId)
    {
        return Run("InstallApo", deviceId);
    }

    public static string Remove()
    {
        return Run("RemoveApo", "");
    }

    public static int SelfTest()
    {
        try
        {
            if (MikrafApoPayloadBytes() != PayloadBytes || Marshal.SizeOf<ApoPayload>() != PayloadBytes)
                return 9;
            return MikrafApoSelfTest();
        }
        catch (DllNotFoundException)
        {
            return 2;
        }
    }

    static string Run(string entry, string deviceId)
    {
        string dll = Path.Combine(AppContext.BaseDirectory, "mikraf-apo.dll");
        if (!File.Exists(dll))
            return "mikraf-apo.dll yok.";
        string resultPath = Path.Combine(ProgramDataDir, "install-result.txt");
        try
        {
            if (File.Exists(resultPath))
                File.Delete(resultPath);
        }
        catch
        {
            // The elevated installer overwrites this file.
        }
        string args = $"\"{dll}\",{entry} {deviceId}";
        try
        {
            using var process = Process.Start(new ProcessStartInfo(Path.Combine(Environment.SystemDirectory, "rundll32.exe"), args)
            {
                UseShellExecute = true,
                Verb = "runas",
            });
            if (process == null)
                return "Kurulum başlatılamadı.";
            process.WaitForExit();
        }
        catch (System.ComponentModel.Win32Exception)
        {
            return "Yönetici izni verilmedi.";
        }
        for (int i = 0; i < 20 && !File.Exists(resultPath); i++)
            Thread.Sleep(100);
        if (!File.Exists(resultPath))
            return "Kurulum sonucu yazılmadı.";
        return File.ReadAllText(resultPath).Trim();
    }

    static ApoPayload FromRack(RackSettings rack)
    {
        var payload = new ApoPayload
        {
            Bypass = rack.Bypass ? 1 : 0,
            SuppressDb = rack.SuppressDb,
            Method = rack.Method == NsMethod.Rnnoise ? 1 : 0,
            ExpRatio = rack.ExpanderRatio,
            ExpThreshold = rack.ExpanderThresholdDb,
            ExpOutput = rack.ExpanderOutputDb,
            ExpAttack = rack.ExpanderAttackMs,
            ExpRelease = rack.ExpanderReleaseMs,
            ExpDetector = rack.ExpanderDetector == Detector.Peak ? 1 : 0,
            EqLow = rack.EqLow,
            EqMid = rack.EqMid,
            EqHigh = rack.EqHigh,
            CompRatio = rack.CompRatio,
            CompThreshold = rack.CompThresholdDb,
            CompOutput = rack.CompOutputDb,
            CompAttack = rack.CompAttackMs,
            CompRelease = rack.CompReleaseMs,
            LimThreshold = rack.LimiterThresholdDb,
            LimRelease = rack.LimiterReleaseMs,
            GainDb = rack.GainDb,
            GateOpen = rack.GateOpenDb,
            GateClose = rack.GateCloseDb,
            GateAttack = rack.GateAttackMs,
            GateHold = rack.GateHoldMs,
            GateRelease = rack.GateReleaseMs,
            UpRatio = rack.UpwardRatio,
            UpThreshold = rack.UpwardThresholdDb,
            UpOutput = rack.UpwardOutputDb,
            UpKnee = rack.UpwardKneeDb,
            UpAttack = rack.UpwardAttackMs,
            UpRelease = rack.UpwardReleaseMs,
            UpDetector = rack.UpwardDetector == Detector.Peak ? 1 : 0,
            NsOn = rack.NoiseSuppress ? 1 : 0,
            GateOn = rack.NoiseGate ? 1 : 0,
            InvertOn = rack.Invert ? 1 : 0,
            GainOn = rack.Gain ? 1 : 0,
            ExpOn = rack.Expander ? 1 : 0,
            UpOn = rack.Upward ? 1 : 0,
            EqOn = rack.Eq ? 1 : 0,
            CompOn = rack.Compressor ? 1 : 0,
            LimOn = rack.Limiter ? 1 : 0,
        };
        var chain = rack.Chain ?? [];
        var codes = new int[12];
        int n = 0;
        foreach (string id in chain)
        {
            int code = Code(id);
            if (code == 0 || n >= codes.Length)
                continue;
            codes[n++] = code;
        }
        payload.ChainCount = n;
        payload.C0 = codes[0];
        payload.C1 = codes[1];
        payload.C2 = codes[2];
        payload.C3 = codes[3];
        payload.C4 = codes[4];
        payload.C5 = codes[5];
        payload.C6 = codes[6];
        payload.C7 = codes[7];
        payload.C8 = codes[8];
        payload.C9 = codes[9];
        payload.C10 = codes[10];
        payload.C11 = codes[11];
        return payload;
    }

    static int Code(string id) => id switch
    {
        ObsFilters.NoiseSuppress => 1,
        ObsFilters.NoiseGate => 2,
        ObsFilters.Invert => 3,
        ObsFilters.Gain => 4,
        ObsFilters.Expander => 5,
        ObsFilters.Upward => 6,
        ObsFilters.Eq => 7,
        ObsFilters.Compressor => 8,
        ObsFilters.Limiter => 9,
        _ => 0,
    };

    static string? ReadDeviceFile()
    {
        string path = Path.Combine(ProgramDataDir, "apo-device.txt");
        if (!File.Exists(path))
            return null;
        try
        {
            byte[] bytes = File.ReadAllBytes(path);
            if (bytes.Length < 4)
                return null;
            string text = System.Text.Encoding.Unicode.GetString(bytes);
            int end = text.IndexOf('\0');
            return end >= 0 ? text[..end] : text.Trim();
        }
        catch
        {
            return null;
        }
    }

    static void TryShare(string dir)
    {
        try
        {
            var info = new DirectoryInfo(dir);
            DirectorySecurity security = info.GetAccessControl();
            security.AddAccessRule(new FileSystemAccessRule(
                new SecurityIdentifier(WellKnownSidType.LocalServiceSid, null),
                FileSystemRights.Modify,
                InheritanceFlags.ContainerInherit | InheritanceFlags.ObjectInherit,
                PropagationFlags.None,
                AccessControlType.Allow));
            security.AddAccessRule(new FileSystemAccessRule(
                new SecurityIdentifier(WellKnownSidType.BuiltinUsersSid, null),
                FileSystemRights.Modify,
                InheritanceFlags.ContainerInherit | InheritanceFlags.ObjectInherit,
                PropagationFlags.None,
                AccessControlType.Allow));
            info.SetAccessControl(security);
        }
        catch
        {
            // The installer sets the same access when it is elevated.
        }
    }

    [DllImport("mikraf-apo.dll", CallingConvention = CallingConvention.Cdecl)]
    static extern int MikrafApoPayloadBytes();

    [DllImport("mikraf-apo.dll", CallingConvention = CallingConvention.Cdecl)]
    static extern int MikrafApoPush(ref ApoPayload payload);

    [DllImport("mikraf-apo.dll", CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Unicode)]
    static extern int MikrafApoQuery(string deviceId);

    [DllImport("mikraf-apo.dll", CallingConvention = CallingConvention.Cdecl)]
    static extern int MikrafApoLive(ref int count, ref int rate, ref int channels);

    [DllImport("mikraf-apo.dll", CallingConvention = CallingConvention.Cdecl)]
    static extern int MikrafApoSelfTest();
}
